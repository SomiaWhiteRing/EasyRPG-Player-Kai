/*
 * This file is part of EasyRPG Player
 *
 * Copyright (c) 2017 EasyRPG Project. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

package org.easyrpg.player.player;

import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipDescription;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;
import android.os.StrictMode;
import android.text.SpannableString;
import android.text.Editable;
import android.text.Spanned;
import android.text.method.LinkMovementMethod;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.text.util.Linkify;
import android.util.Log;
import android.view.KeyEvent;
import android.view.InputDevice;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RelativeLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.window.OnBackInvokedDispatcher;

import androidx.annotation.NonNull;
import androidx.core.content.FileProvider;
import androidx.core.view.GravityCompat;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.drawerlayout.widget.DrawerLayout;

import com.google.android.material.navigation.NavigationView;

import org.easyrpg.player.Helper;
import org.easyrpg.player.R;
import org.easyrpg.player.button_mapping.ButtonMappingActivity;
import org.easyrpg.player.button_mapping.InputLayout;
import org.easyrpg.player.button_mapping.VirtualButton;
import org.easyrpg.player.game_browser.Game;
import org.easyrpg.player.game_browser.GameBrowserActivity;
import org.easyrpg.player.settings.SettingsManager;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.lang.reflect.Method;
import java.text.BreakIterator;
import java.util.ArrayList;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;

/**
 * EasyRPG Player for Android (inheriting from SDLActivity)
 */
public class EasyRpgPlayerActivity extends SDLActivity implements NavigationView.OnNavigationItemSelectedListener {
    public static final String TAG_PROJECT_PATH = "project_path";
    public static final String TAG_LOG_FILE = "log_file";
    public static final String TAG_SAVE_PATH = "save_path";
    public static final String TAG_COMMAND_LINE = "command_line";
    public static final String TAG_STANDALONE = "standalone_mode";
    public static final int LAYOUT_EDIT = 12345;

    public static boolean samsungMultitouchWorkaround = false;
    public static int pointerCount = 0;

    DrawerLayout drawer;
    InputLayout inputLayout;
    private boolean uiVisible = true;

    private int topInset = 0;

    private static class NameInputRequest {
        volatile boolean finished;
        String value;
        AlertDialog dialog;
        boolean imeWasVisible;
    }

    private volatile NameInputRequest nameInputRequest;
    private volatile boolean physicalTextKeyboard;
    private InputManager nameKeyboardDevices;
    private final InputManager.InputDeviceListener nameKeyboardListener = new InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { refreshPhysicalTextKeyboard(); }
        @Override public void onInputDeviceRemoved(int id) { refreshPhysicalTextKeyboard(); }
        @Override public void onInputDeviceChanged(int id) { refreshPhysicalTextKeyboard(); }
    };

    public boolean hasPhysicalTextKeyboard() {
        return physicalTextKeyboard;
    }

    private void refreshPhysicalTextKeyboard() {
        boolean found = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual()
                    && (Build.VERSION.SDK_INT < 27 || device.isEnabled())
                    && device.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC
                    && (device.getSources() & InputDevice.SOURCE_KEYBOARD) == InputDevice.SOURCE_KEYBOARD) {
                found = true; break;
            }
        }
        physicalTextKeyboard = found;
    }

    private void finishNameInput(NameInputRequest request, String value) {
        if (nameInputRequest != request || request.finished) return;
        request.value = value;
        request.finished = true;
        if (request.dialog != null) request.dialog.dismiss();
        if (!consoleInputClosed && mSurface != null) mSurface.requestFocus();
        updateConsoleFocus();
    }

    public boolean beginNameInput(String initial) {
        if (consoleInputClosed || nameInputRequest != null) return false;
        NameInputRequest request = new NameInputRequest();
        nameInputRequest = request;
        runOnUiThread(() -> {
            if (nameInputRequest != request) return;
            if (isFinishing() || isDestroyed()) { finishNameInput(request, null); return; }
            EditText input = new EditText(this) {
                @Override public boolean onKeyPreIme(int keyCode, KeyEvent event) {
                    if (keyCode == KeyEvent.KEYCODE_BACK) {
                        if (event.getAction() == KeyEvent.ACTION_UP) finishNameInput(request, null);
                        return true;
                    }
                    return super.onKeyPreIme(keyCode, event);
                }
            };
            input.setSingleLine(true);
            input.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
            input.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            input.setText(initial);
            input.setSelection(input.length());
            AlertDialog dialog = new AlertDialog.Builder(this)
                    .setTitle(R.string.name_input_title).setView(input)
                    .setPositiveButton(android.R.string.ok, (unused, which) -> finishNameInput(request, input.getText().toString()))
                    .setNegativeButton(android.R.string.cancel, (unused, which) -> finishNameInput(request, null)).create();
            request.dialog = dialog;
            input.setOnEditorActionListener((view, action, event) -> {
                if (action == EditorInfo.IME_ACTION_DONE) {
                    finishNameInput(request, input.getText().toString()); return true;
                }
                return false;
            });
            dialog.setOnDismissListener(unused -> finishNameInput(request, null));
            dialog.setOnShowListener(unused -> {
                dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                        | WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
                ViewCompat.setOnApplyWindowInsetsListener(dialog.getWindow().getDecorView(), (view, insets) -> {
                    boolean visible = insets.isVisible(WindowInsetsCompat.Type.ime());
                    if (visible) request.imeWasVisible = true;
                    else if (request.imeWasVisible) finishNameInput(request, null);
                    return insets;
                });
                input.requestFocus();
                input.post(() -> {
                    if (nameInputRequest == request && !request.finished) {
                        InputMethodManager keyboard = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                        if (keyboard != null) keyboard.showSoftInput(input, InputMethodManager.SHOW_IMPLICIT);
                    }
                });
            });
            dialog.show();
        });
        return true;
    }

    // null = pending, [] = cancelled, [text] = submitted (including an empty name).
    public String[] pollNameInput() {
        NameInputRequest request = nameInputRequest;
        if (request == null) return new String[0];
        if (!request.finished) return null;
        nameInputRequest = null;
        return request.value == null ? new String[0] : new String[]{request.value};
    }

    public void cancelNameInput() {
        NameInputRequest request = nameInputRequest;
        nameInputRequest = null;
        if (request != null) runOnUiThread(() -> {
            if (request.dialog != null) request.dialog.dismiss();
        });
    }

    private static class ConsoleRequest {
        final CountDownLatch done = new CountDownLatch(1);
        String value;
        int result;
    }

    private volatile ConsoleRequest consoleRead, consoleCommand;
    private volatile boolean consoleInputClosed;
    private volatile int consoleGeneration;
    private LinearLayout consolePanel, consoleInputRow;
    private TextView consoleCaption, consoleOutput;
    private ScrollView consoleScroll;
    private EditText consoleInput;
    private int consoleForeground = 7, consoleBackground = 0;
    private int consoleRowStart, consoleCursor, consoleColumn, consoleColumns;
    private final BreakIterator consoleGraphemes = BreakIterator.getCharacterInstance(Locale.ROOT);
    private ViewTreeObserver.OnGlobalFocusChangeListener consoleFocusListener;
    private View.OnLayoutChangeListener consoleLayoutListener;
    private static final int[] CONSOLE_COLORS = {0xff000000, 0xff000080, 0xff008000, 0xff008080,
            0xff800000, 0xff800080, 0xff808000, 0xffc0c0c0, 0xff808080, 0xff0000ff,
            0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff};

    private boolean consoleHasFocus() {
        return consolePanel != null && consolePanel.findFocus() != null;
    }

    private void updateConsoleFocus() {
        if (!consoleInputClosed && !mBrokenLibraries)
            SDLActivity.nativeFocusChanged(hasWindowFocus() && !consoleHasFocus());
    }

    private void restoreConsoleGameFocus() {
        if (consoleInput != null) {
            InputMethodManager keyboard = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (keyboard != null) keyboard.hideSoftInputFromWindow(consoleInput.getWindowToken(), 0);
        }
        if (!consoleInputClosed && mSurface != null) mSurface.requestFocus();
        updateConsoleFocus();
    }

    private void layoutConsolePanel() {
        if (consolePanel == null) return;
        float density = getResources().getDisplayMetrics().density;
        int width = mLayout.getWidth() > 0 ? mLayout.getWidth() : getResources().getDisplayMetrics().widthPixels;
        int height = mLayout.getHeight() > 0 ? mLayout.getHeight() : getResources().getDisplayMetrics().heightPixels;
        RelativeLayout.LayoutParams layout = new RelativeLayout.LayoutParams(
                Math.min((int) (640 * density), width * 9 / 10),
                Math.min((int) (480 * density), height * 2 / 3));
        layout.addRule(RelativeLayout.ALIGN_PARENT_RIGHT);
        layout.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        int margin = (int) (8 * density);
        layout.setMargins(margin, topInset + margin, margin, margin);
        consolePanel.setLayoutParams(layout);
        consolePanel.bringToFront();
    }

    private int consoleGlyphColumns(String text, int start, int end) {
        float space = consoleOutput.getPaint().measureText(" ");
        return Math.max(1, Math.round(consoleOutput.getPaint().measureText(text, start, end) / space));
    }

    private static String consoleSpaces(int count) {
        char[] spaces = new char[count]; java.util.Arrays.fill(spaces, ' '); return new String(spaces);
    }

    private void styleConsolePadding(SpannableString run, int start, int end, Editable buffer, int old) {
        if (start == end) return;
        int foreground = CONSOLE_COLORS[7], background = CONSOLE_COLORS[0];
        if (old >= consoleRowStart && old < buffer.length()) {
            ForegroundColorSpan[] fg = buffer.getSpans(old, old + 1, ForegroundColorSpan.class);
            BackgroundColorSpan[] bg = buffer.getSpans(old, old + 1, BackgroundColorSpan.class);
            if (fg.length > 0) foreground = fg[fg.length - 1].getForegroundColor();
            if (bg.length > 0) background = bg[bg.length - 1].getBackgroundColor();
        }
        run.setSpan(new ForegroundColorSpan(foreground), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        run.setSpan(new BackgroundColorSpan(background), start, end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }

    private void writeConsoleGlyphRun(String text) {
        Editable buffer = consoleOutput.getEditableText();
        BreakIterator glyphs = (BreakIterator) consoleGraphemes.clone(); glyphs.setText(text);
        int width = 0;
        for (int start = glyphs.first(), end; (end = glyphs.next()) != BreakIterator.DONE; start = end)
            width += consoleGlyphColumns(text, start, end);
        String row = buffer.subSequence(consoleRowStart, buffer.length()).toString();
        int first = row.length(), last = row.length(), firstColumn = 0, lastColumn = 0, column = 0;
        glyphs.setText(row);
        for (int start = glyphs.first(), end; (end = glyphs.next()) != BreakIterator.DONE; start = end) {
            int next = column + consoleGlyphColumns(row, start, end);
            if (next > consoleColumn && column < consoleColumn + width) {
                if (first == row.length()) { first = start; firstColumn = column; }
                last = end; lastColumn = next;
            }
            column = next;
        }
        if (first == row.length()) firstColumn = lastColumn = column;
        int left = Math.max(0, consoleColumn - firstColumn), right = Math.max(0, lastColumn - consoleColumn - width);
        // Preserve the untouched cells and colors when only half of a wide glyph is overwritten.
        SpannableString run = new SpannableString(consoleSpaces(left) + text + consoleSpaces(right));
        run.setSpan(new ForegroundColorSpan(CONSOLE_COLORS[consoleForeground]), left, left + text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        run.setSpan(new BackgroundColorSpan(CONSOLE_COLORS[consoleBackground]), left, left + text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        glyphs.setText(text);
        for (int start = glyphs.first(), end; (end = glyphs.next()) != BreakIterator.DONE; start = end) {
            float natural = consoleOutput.getPaint().measureText(text, start, end);
            int advance = Math.round(consoleGlyphColumns(text, start, end) * consoleOutput.getPaint().measureText(" "));
            if (Math.abs(natural - advance) > 0.01f) {
                // Fallback CJK fonts can be narrower than two monospace cells.
                run.setSpan(new android.text.style.ReplacementSpan() {
                    @Override public int getSize(android.graphics.Paint paint, CharSequence value, int from, int to, android.graphics.Paint.FontMetricsInt metrics) {
                        if (metrics != null) paint.getFontMetricsInt(metrics);
                        return advance;
                    }
                    @Override public void draw(android.graphics.Canvas canvas, CharSequence value, int from, int to, float x, int top, int y, int bottom, android.graphics.Paint paint) {
                        canvas.drawText(value, from, to, x, y, paint);
                    }
                }, left + start, left + end, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
            }
        }
        styleConsolePadding(run, 0, left, buffer, consoleRowStart + first);
        styleConsolePadding(run, left + text.length(), run.length(), buffer, consoleRowStart + last - 1);
        buffer.replace(consoleRowStart + first, consoleRowStart + last, run);
        consoleCursor = consoleRowStart + first + left + text.length(); consoleColumn += width;
    }

    private void nextConsoleRow() {
        Editable buffer = consoleOutput.getEditableText();
        buffer.append("\n"); consoleRowStart = consoleCursor = buffer.length(); consoleColumn = 0;
        consoleGraphemes.setText("");
    }

    private void writeConsoleRun(String text) {
        float space = consoleOutput.getPaint().measureText(" ");
        if (consoleColumns == 0) {
            int width = consoleOutput.getWidth();
            if (width == 0) width = consolePanel.getLayoutParams().width
                    - consolePanel.getPaddingLeft() - consolePanel.getPaddingRight();
            width -= consoleOutput.getCompoundPaddingLeft() + consoleOutput.getCompoundPaddingRight();
            consoleColumns = Math.max(1, (int) Math.floor(width / space));
        }
        int width = consoleColumns, used = consoleColumn;
        BreakIterator glyphs = (BreakIterator) consoleGraphemes.clone(); glyphs.setText(text);
        int runStart = 0;
        for (int start = glyphs.first(), end; (end = glyphs.next()) != BreakIterator.DONE; start = end) {
            int advance = consoleGlyphColumns(text, start, end);
            // Filling the last column delays wrapping until the next printable glyph.
            if (used > 0 && used + advance > width + 0.01f) {
                if (start > runStart) writeConsoleGlyphRun(text.substring(runStart, start));
                nextConsoleRow(); runStart = start; used = 0;
            }
            used += advance;
        }
        if (runStart < text.length()) writeConsoleGlyphRun(text.substring(runStart));
    }

    private void appendConsoleText(String text) {
        if (consoleOutput == null || text.isEmpty()) return;
        Editable buffer = consoleOutput.getEditableText();
        if (consoleColumns == 0) writeConsoleRun("");
        boolean bell = false;
        for (int i = 0; i < text.length();) {
            char character = text.charAt(i++);
            switch (character) {
                case '\r': consoleCursor = consoleRowStart; consoleColumn = 0; break;
                case '\n':
                    nextConsoleRow(); break;
                case '\b':
                    if (consoleColumn > 0) {
                        String row = buffer.subSequence(consoleRowStart, buffer.length()).toString();
                        consoleGraphemes.setText(row); int column = 0; boolean found = false;
                        for (int start = consoleGraphemes.first(), end; (end = consoleGraphemes.next()) != BreakIterator.DONE; start = end) {
                            int next = column + consoleGlyphColumns(row, start, end);
                            if (next >= consoleColumn) { consoleColumn = column; consoleCursor = consoleRowStart + start; found = true; break; }
                            column = next;
                        }
                        if (!found) { --consoleColumn; consoleCursor = buffer.length(); }
                    }
                    break;
                case '\t':
                    consoleColumn += Math.min(consoleColumns - consoleColumn, 8 - (consoleColumn & 7));
                    consoleCursor = buffer.length(); break;
                case 7: bell = true; break;
                case 0: writeConsoleRun(" "); break;
                default:
                    int start = i - 1;
                    while (i < text.length()) {
                        char next = text.charAt(i);
                        if (next == 0 || (next >= 7 && next <= 10) || next == '\r') break;
                        ++i;
                    }
                    writeConsoleRun(text.substring(start, i));
            }
        }
        // No OS console window on Android; a visual bell preserves game/audio focus.
        if (bell) { consolePanel.setAlpha(0.5f); consolePanel.animate().alpha(1).setDuration(150).start(); }
        ScrollView scroll = consoleScroll;
        scroll.post(() -> scroll.scrollTo(0, scroll.getChildAt(0).getHeight()));
    }

    private void finishConsoleRead(String value) {
        ConsoleRequest request = consoleRead;
        consoleRead = null;
        try {
            if (consoleInputRow != null) consoleInputRow.setVisibility(View.GONE);
            if (request != null) {
                request.value = value;
                if (value != null) appendConsoleText(value);
            }
            restoreConsoleGameFocus();
        } finally {
            if (request != null) request.done.countDown();
        }
    }

    private void closeConsolePanel() {
        if (consolePanel == null) {
            if (consoleRead != null) finishConsoleRead(null);
            return;
        }
        finishConsoleRead(null);
        getWindow().getDecorView().getViewTreeObserver().removeOnGlobalFocusChangeListener(consoleFocusListener);
        mLayout.removeOnLayoutChangeListener(consoleLayoutListener);
        consolePanel.animate().cancel();
        mLayout.removeView(consolePanel);
        consolePanel = null; consoleInputRow = null; consoleCaption = null;
        consoleOutput = null; consoleScroll = null; consoleInput = null; consoleFocusListener = null;
        consoleLayoutListener = null;
        consoleRowStart = consoleCursor = consoleColumn = consoleColumns = 0; consoleGraphemes.setText("");
        consoleGeneration++;
        restoreConsoleGameFocus();
    }

    private void openConsolePanel(String title) {
        consoleForeground = 7; consoleBackground = 0;
        consolePanel = new LinearLayout(this);
        consolePanel.setOrientation(LinearLayout.VERTICAL);
        consolePanel.setFocusableInTouchMode(true);
        consolePanel.setBackgroundColor(CONSOLE_COLORS[0]);
        int padding = (int) (8 * getResources().getDisplayMetrics().density);
        consolePanel.setPadding(padding, padding, padding, padding);
        LinearLayout header = new LinearLayout(this);
        consoleCaption = new TextView(this); consoleCaption.setText(title); consoleCaption.setTextColor(CONSOLE_COLORS[7]);
        Button close = new Button(this); close.setText("\u00d7"); close.setContentDescription("Close console");
        close.setOnClickListener(view -> closeConsolePanel());
        header.addView(consoleCaption, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        header.addView(close); consolePanel.addView(header);
        consoleScroll = new ScrollView(this);
        consoleOutput = new TextView(this); consoleOutput.setTypeface(Typeface.MONOSPACE);
        consoleOutput.setHorizontallyScrolling(true);
        consoleOutput.setTextColor(CONSOLE_COLORS[7]); consoleOutput.setTextIsSelectable(true);
        consoleOutput.setText("", TextView.BufferType.EDITABLE); consoleRowStart = consoleCursor = consoleColumn = consoleColumns = 0;
        consoleScroll.addView(consoleOutput);
        consolePanel.addView(consoleScroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        consoleInputRow = new LinearLayout(this); consoleInputRow.setVisibility(View.GONE);
        consoleInput = new EditText(this); consoleInput.setSingleLine(true);
        consoleInput.setTypeface(Typeface.MONOSPACE); consoleInput.setTextColor(CONSOLE_COLORS[7]);
        consoleInput.setBackgroundColor(CONSOLE_COLORS[0]);
        consoleInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        consoleInputRow.addView(consoleInput, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Button submit = new Button(this); submit.setText(R.string.ok);
        submit.setOnClickListener(view -> finishConsoleRead(consoleInput.getText().toString() + "\r\n"));
        Button cancel = new Button(this); cancel.setText(R.string.cancel);
        cancel.setOnClickListener(view -> finishConsoleRead(null));
        consoleInputRow.addView(submit); consoleInputRow.addView(cancel); consolePanel.addView(consoleInputRow);
        consoleInput.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_DONE || (event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                    && event.getAction() == KeyEvent.ACTION_DOWN)) {
                finishConsoleRead(consoleInput.getText().toString() + "\r\n"); return true;
            }
            return false;
        });
        mLayout.addView(consolePanel); layoutConsolePanel();
        consoleGeneration++;
        consoleFocusListener = (oldView, newView) -> updateConsoleFocus();
        getWindow().getDecorView().getViewTreeObserver().addOnGlobalFocusChangeListener(consoleFocusListener);
        consoleLayoutListener = (view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            if (right - left != oldRight - oldLeft || bottom - top != oldBottom - oldTop) layoutConsolePanel();
        };
        mLayout.addOnLayoutChangeListener(consoleLayoutListener);
        if (hasWindowFocus()) restoreConsoleGameFocus();
    }

    private int executeConsoleCommand(String op, String text, int options, int foreground, int background) {
        if (op.equals("close")) { closeConsolePanel(); return 1; }
        if (op.equals("open")) {
            if (consolePanel == null) openConsolePanel(text);
            else if (options != 0) { consoleCaption.setText(text); layoutConsolePanel(); }
            return 1;
        }
        if (consolePanel == null) return 0;
        switch (op) {
            case "write": appendConsoleText(text); break;
            case "color":
                if ((options & 8) != 0) consoleForeground = foreground & 15;
                if ((options & 16) != 0) consoleBackground = background & 15;
                consoleInput.setTextColor(CONSOLE_COLORS[consoleForeground]);
                consoleInput.setBackgroundColor(CONSOLE_COLORS[consoleBackground]);
                break;
            case "focus":
                if (hasWindowFocus()) {
                    if (options != 0 && !consoleHasFocus()) consolePanel.requestFocus();
                    else if (options == 0 && consoleHasFocus()) restoreConsoleGameFocus();
                }
                break;
            default: return 0;
        }
        return 1;
    }

    /** Called synchronously by the SDL thread through JNI. */
    public int commandManiacConsole(String op, String text, int options, int foreground, int background) {
        if (op.equals("generation")) return consoleGeneration;
        if (Looper.myLooper() == Looper.getMainLooper() || consoleInputClosed) return 0;
        ConsoleRequest request = new ConsoleRequest(); consoleCommand = request;
        if (consoleInputClosed) { consoleCommand = null; return 0; }
        runOnUiThread(() -> {
            try {
                if (consoleCommand == request && !consoleInputClosed && !isFinishing() && !isDestroyed())
                    request.result = executeConsoleCommand(op, text, options, foreground, background);
            } catch (RuntimeException exception) {
                Log.w("EasyRPG", "Console command unavailable", exception); closeConsolePanel();
            } finally {
                if (consoleCommand == request) consoleCommand = null;
                request.done.countDown();
            }
        });
        try { request.done.await(); return consoleInputClosed ? 0 : request.result; }
        catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            if (consoleCommand == request) consoleCommand = null;
            return 0;
        }
    }

    /** Called synchronously by the SDL thread through JNI. */
    public String readManiacConsole() {
        if (Looper.myLooper() == Looper.getMainLooper() || consoleInputClosed) return null;
        ConsoleRequest request = new ConsoleRequest();
        consoleRead = request;
        // Publish before rechecking destruction, so onDestroy can release the waiter.
        if (consoleInputClosed) {
            consoleRead = null;
            return null;
        }
        runOnUiThread(() -> {
            if (consoleRead != request || consoleInputClosed || consolePanel == null || isFinishing() || isDestroyed()) {
                if (consoleRead == request) consoleRead = null;
                request.done.countDown();
                return;
            }
            try {
                consoleInput.setText(""); consoleInputRow.setVisibility(View.VISIBLE); consoleInput.requestFocus();
                InputMethodManager keyboard = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                if (keyboard != null) keyboard.showSoftInput(consoleInput, InputMethodManager.SHOW_IMPLICIT);
            } catch (RuntimeException exception) {
                Log.w("EasyRPG", "Console input unavailable", exception);
                if (consoleRead == request) finishConsoleRead(null);
                request.done.countDown();
            }
        });
        try {
            request.done.await();
            return consoleInputClosed ? null : request.value;
        } catch (InterruptedException exception) {
            Thread.currentThread().interrupt();
            runOnUiThread(() -> {
                if (consoleRead == request) finishConsoleRead(null);
            });
            return null;
        }
    }

    @Override
    protected void onDestroy() {
        consoleInputClosed = true; consoleGeneration++;
        cancelNameInput();
        if (nameKeyboardDevices != null) nameKeyboardDevices.unregisterInputDeviceListener(nameKeyboardListener);
        ConsoleRequest read = consoleRead; consoleRead = null;
        if (read != null) { read.value = null; read.done.countDown(); }
        ConsoleRequest request = consoleCommand;
        consoleCommand = null;
        if (request != null) {
            request.result = 0;
            request.done.countDown();
        }
        // SDL destruction may join the native thread, which must already be unblocked.
        try { closeConsolePanel(); } finally { super.onDestroy(); }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && consoleHasFocus()) updateConsoleFocus();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "easyrpg_android"
        };
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new EasyRpgSurface(context, this);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries) {
            return;
        }

        SettingsManager.init(getApplicationContext());
        refreshPhysicalTextKeyboard();
        nameKeyboardDevices = (InputManager) getSystemService(INPUT_SERVICE);
        if (nameKeyboardDevices != null) nameKeyboardDevices.registerInputDeviceListener(nameKeyboardListener, null);

        // Load our custom activity layout
        setContentView(R.layout.activity_player);

        // Menu configuration
        this.drawer = findViewById(R.id.drawer_layout);
        drawer.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED);
        drawer.addDrawerListener(new DrawerLayout.DrawerListener() {
            @Override
            public void onDrawerSlide(View drawerView, float slideOffset) {
                // Prevent drawer from rendering behind the SDL Surface
                // via https://stackoverflow.com/q/20976149/
                drawer.bringChildToFront(drawerView);
                drawer.requestLayout();
            }

            @Override
            public void onDrawerOpened(View drawerView) {

            }

            @Override
            public void onDrawerClosed(View drawerView) {

            }

            @Override
            public void onDrawerStateChanged(int newState) {

            }
        });

        NavigationView navigationView = findViewById(R.id.nav_view);
        navigationView.setNavigationItemSelectedListener(this);

        applyRequestedOrientation();

        // Put the gameScreen
        mLayout = findViewById(R.id.main_layout);

        ViewGroup parent = (ViewGroup) mSurface.getParent();
        if (parent != null) {
            parent.removeView(mSurface);
        }
        mLayout.addView(mSurface);

        applyFullscreenMode();
        updateScreenPosition();

        if (Build.VERSION.SDK_INT >= 35) {
            getWindow().getDecorView().setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @NonNull
                @Override
                public WindowInsets onApplyWindowInsets(@NonNull View view, @NonNull WindowInsets insets) {
                    Insets systemInsets = insets.getInsetsIgnoringVisibility(WindowInsets.Type.systemBars());
                    topInset = systemInsets.top;
                    showInputLayout();
                    return insets;
                }
            });
        } else {
            showInputLayout();
        }

        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                () -> {
                    backPressed();
                }
            );
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (ev.getActionMasked() == MotionEvent.ACTION_DOWN && consoleHasFocus()) {
            int[] position = new int[2]; consolePanel.getLocationOnScreen(position);
            float x = ev.getRawX() - position[0], y = ev.getRawY() - position[1];
            if (x < 0 || y < 0 || x >= consolePanel.getWidth() || y >= consolePanel.getHeight()) restoreConsoleGameFocus();
        }
        pointerCount = ev.getPointerCount();
        return super.dispatchTouchEvent(ev);
    }

    /**
     * Called after a screen orientation change
     */
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        applyFullscreenMode();
        updateScreenPosition();
        showInputLayout();
        layoutConsolePanel();
    }

    @Override
    protected void onPause() {
        NameInputRequest request = nameInputRequest;
        if (request != null) finishNameInput(request, null);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyRequestedOrientation();
        applyFullscreenMode();
        updateScreenPosition();
    }

    @Override
    public boolean onNavigationItemSelected(MenuItem item) {
        // Handle item selection
        if (item.getItemId() == R.id.open_settings) {
            openSettings();
        } else if (item.getItemId() == R.id.toggle_ui) {
            uiVisible = !uiVisible;
            showInputLayout();
        } else if (item.getItemId() == R.id.toggle_fps) {
            toggleFps();
        } else if (item.getItemId() == R.id.edit_layout) {
            editLayout();
        } else if (item.getItemId() == R.id.report_bug) {
            reportBug();
        } else if (item.getItemId() == R.id.reset_game) {
            showResetGameDialog();
        } else if (item.getItemId() == R.id.end_game) {
            showEndGameDialog();
        }
        openOrCloseMenu();
        return false;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == LAYOUT_EDIT) {
            showInputLayout();
        }
    }

    private void editLayout() {
        Intent intent = new Intent(this, org.easyrpg.player.button_mapping.ButtonMappingActivity.class);

        // Choose the proper InputLayout
        int display_mode = getResources().getConfiguration().orientation;
        if (display_mode == Configuration.ORIENTATION_LANDSCAPE) {
            intent.putExtra(ButtonMappingActivity.TAG_ORIENTATION, ButtonMappingActivity.TAG_ORIENTATION_VALUE_HORIZONTAL);
        } else {
            intent.putExtra(ButtonMappingActivity.TAG_ORIENTATION, ButtonMappingActivity.TAG_ORIENTATION_VALUE_VERTICAL);
        }

        for (VirtualButton v : inputLayout.getButtonList()) {
            mLayout.removeView(v);
        }

        startActivityForResult(intent, LAYOUT_EDIT);
    }

    private void reportBug() {
        AlertDialog.Builder alertDialogBuilder = new AlertDialog.Builder(this);
        alertDialogBuilder.setTitle(R.string.app_name);

        final SpannableString bug_msg = new SpannableString(getApplicationContext().getString(R.string.report_bug_msg));
        Linkify.addLinks(bug_msg, Linkify.ALL);

        // set dialog message
        alertDialogBuilder.setMessage(bug_msg).setCancelable(false)
                .setPositiveButton(R.string.ok, (dialog, id) -> {
                    // Attach to the email: the easyrpg log file and savefiles
                    ArrayList<Uri> files = new ArrayList<>();

                    String savepath = getIntent().getStringExtra(TAG_SAVE_PATH);

                    if (getIntent().getBooleanExtra(TAG_STANDALONE, false)) {
                        File logFile = new File(getIntent().getStringExtra(TAG_LOG_FILE));
                        if (logFile.exists()) {
                            Uri logUri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", logFile);
                            if (logUri != null) {
                                files.add(logUri);
                            }
                        }

                        for (int i = 1; i <= 15; ++i) {
                            File saveFile = new File(savepath, String.format(Locale.ROOT, "Save%02d.lsd", i));
                            if (saveFile.exists()) {
                                Uri saveUri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", saveFile);
                                if (saveUri != null) {
                                    files.add(saveUri);
                                }
                            }
                        }
                    } else {
                        // Must be properly URI encoded
                        SafFile logFile = SafFile.fromPath(getContext(), getIntent().getStringExtra(TAG_LOG_FILE));
                        if (logFile != null) {
                            files.add(logFile.getUri());
                        }

                        Uri saveFolder = Uri.parse(savepath);

                        // The save files
                        files.addAll(Helper.findFileUriWithRegex(getContext(), saveFolder, ".*lsd"));
                    }

                    if (Build.VERSION.SDK_INT >= 24) {
                        // Lazy workaround as suggested on https://stackoverflow.com/q/38200282
                        try {
                            Method m = StrictMode.class.getMethod("disableDeathOnFileUriExposure");
                            m.invoke(null);
                        } catch (Exception e) {
                            Log.i("EasyRPG", "Bug report: Calling disableDeathOnFileUriExposure failed");
                        }
                    }

                    Intent intent = new Intent(Intent.ACTION_SEND_MULTIPLE);
                    // intent.setData(Uri.parse("mailto:"));
                    intent.setType(ClipDescription.MIMETYPE_TEXT_PLAIN);
                    intent.putExtra(Intent.EXTRA_EMAIL, new String[]{"easyrpg@easyrpg.org"});
                    intent.putExtra(Intent.EXTRA_SUBJECT, "Bug report");
                    intent.putExtra(Intent.EXTRA_TEXT, getApplicationContext().getString(R.string.report_bug_mail));
                    intent.putExtra(Intent.EXTRA_STREAM, files);
                    try {
                        startActivity(intent);
                    } catch (ActivityNotFoundException e) {
                        Log.e("EasyRPG", "No Mail App found");
                    }
                }).setNegativeButton(R.string.cancel, (dialog, id) -> dialog.cancel());

        AlertDialog alertDialog = alertDialogBuilder.create();

        alertDialog.show();

        ((TextView) alertDialog.findViewById(android.R.id.message)).setMovementMethod(LinkMovementMethod.getInstance());
    }

    public void backPressed() {
        if (nameInputRequest != null && !nameInputRequest.finished) { finishNameInput(nameInputRequest, null); return; }
        if (consoleRead != null) { finishConsoleRead(null); return; }
        if (consoleHasFocus()) { closeConsolePanel(); return; }
        openOrCloseMenu();
    }

    @Override
    // API 33+ uses the registered OnBackInvokedCallback; SDL still needs key events below 33.
    @SuppressLint("GestureBackNavigation")
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (consoleHasFocus()) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
                if (event.getAction() == KeyEvent.ACTION_UP) backPressed();
                return true;
            }
            // Route IME/hardware keys to the panel rather than the SDL Surface.
            if (event.getKeyCode() != KeyEvent.KEYCODE_VOLUME_UP && event.getKeyCode() != KeyEvent.KEYCODE_VOLUME_DOWN) {
                getWindow().superDispatchKeyEvent(event); return true;
            }
        }
        if (Build.VERSION.SDK_INT >= 33) {
            // Uses OnBackInvokedDispatcher
            return super.dispatchKeyEvent(event);
        }

        // Handle back button on older Androids
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            backPressed();
            return false;
        }

        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (consoleHasFocus() && (event.getSource() & android.view.InputDevice.SOURCE_JOYSTICK)
                == android.view.InputDevice.SOURCE_JOYSTICK) return true;
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // Filter orientation events generated by SDL (hint is empty)
        if (hint.isEmpty()) {
            return;
        }

        if (SettingsManager.isForcedLandscape()) {
            applyRequestedOrientation();
            return;
        }

        super.setOrientationBis(w, h, resizable, hint);
    }

    private void applyRequestedOrientation() {
        if (SettingsManager.isForcedLandscape()) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
        } else {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_USER);
        }
    }

    public void applyFullscreenMode() {
        if (!SettingsManager.isFullscreen()) {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_VISIBLE);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            return;
        }

        int flags = View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE;

        getWindow().getDecorView().setSystemUiVisibility(flags);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
    }

    /**
     * This function permit to open the menu, in a static way
     */
    public static void staticOpenOrCloseMenu(EasyRpgPlayerActivity activity) {
        if (activity != null) {
            activity.openOrCloseMenu();
        }
    }

    public void openOrCloseMenu() {
        if (drawer.isDrawerOpen(GravityCompat.START)) {
            drawer.closeDrawer(GravityCompat.START);
        } else {
            drawer.openDrawer(GravityCompat.START);
        }
    }

    private void showResetGameDialog() {
        AlertDialog.Builder alertDialogBuilder = new AlertDialog.Builder(this);
        alertDialogBuilder.setTitle(R.string.app_name);

        // set dialog message
        alertDialogBuilder.setMessage(R.string.do_want_reset).setCancelable(false)
            .setPositiveButton(R.string.yes, (dialog, id) -> resetGame()).setNegativeButton(R.string.no, (dialog, id) -> dialog.cancel());

        // create alert dialog
        AlertDialog alertDialog = alertDialogBuilder.create();

        alertDialog.show();
    }

    private void showEndGameDialog() {
        AlertDialog.Builder alertDialogBuilder = new AlertDialog.Builder(this);
        alertDialogBuilder.setTitle(R.string.app_name);

        // set dialog message
        alertDialogBuilder.setMessage(R.string.do_want_quit).setCancelable(false)
                .setPositiveButton(R.string.yes, (dialog, id) -> endGame()).setNegativeButton(R.string.no, (dialog, id) -> dialog.cancel());

        // create alert dialog
        AlertDialog alertDialog = alertDialogBuilder.create();

        alertDialog.show();
    }

    public static native void openSettings();

    public static native void endGame();

    public static native void resetGame();

    public static native void toggleFps();

    protected String[] getArguments() {
        return getIntent().getStringArrayExtra(TAG_COMMAND_LINE);
    }

    /**
     * Used to retrieve the selected game in the browser.
     *
     * @return Full path to game
     */
    public Game getProjectPath() {
        return GameBrowserActivity.getSelectedGame();
    }

    /**
     * Used by the native code to retrieve the RTP directory. Invoked via JNI.
     * DO NOT DELETE, IT IS USED BY THE NATIVE CODE (the warning is a lie)
     *
     * @return Full path to the RTP
     */
    public String getRtpPath() {
        if (SettingsManager.isRTPScanningEnabled()) {
            Uri rtpFolderURI = SettingsManager.getRTPFolderURI(this);
            if (rtpFolderURI != null) {
                return rtpFolderURI.toString();
            }
        }
        return "";
    }

    public static SafFile getHandleForPath(String path) {
        return SafFile.fromPath(getContext(), path);
    }

    public void showInputLayout() {
        // Remove all buttons from the UI
        if (inputLayout != null) {
            for (VirtualButton b : inputLayout.getButtonList()) {
                mLayout.removeView(b);
            }
        }

        if (uiVisible) {
            // Retrieve the proper layout depending on the screen orientation
            int display_mode = getResources().getConfiguration().orientation;
            if (display_mode == Configuration.ORIENTATION_LANDSCAPE) {
                this.inputLayout = SettingsManager.getInputLayoutHorizontal(this);
            } else {
                this.inputLayout = SettingsManager.getInputLayoutVertical(this);
            }

            for (VirtualButton b : inputLayout.getButtonList()) {
                // We add it, if it's not the case already
                if (b.getParent() != mLayout) {
                    if (b.getParent() != null) {
                        ((ViewGroup) b.getParent()).removeAllViews();
                    }
                    mLayout.addView(b);
                }
            }

            for (VirtualButton b : this.inputLayout.getButtonList()) {
                Helper.setLayoutPosition(this, b, b.getPosX(), b.getPosY());
            }
        }
    }

    public void updateScreenPosition() {
        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT);

        params.topMargin = SettingsManager.isFullscreen() ? 0 : topInset;
        params.leftMargin = 0;
        mSurface.setLayoutParams(params);
        mSurface.requestLayout();
    }

    /**
     * Used by the native APK-FS code to retrieve a handle to the asset manager.
     *
     * @return asset manager
     */
    public static AssetManager getAssetManager() {
        return getContext().getAssets();
    }

    /**
     * Called after the activity is being re-displayed
     */
    @Override
    public void onRestart() {
        super.onRestart();
        applyRequestedOrientation();
        applyFullscreenMode();
        updateScreenPosition();
        showInputLayout();
    }
}
