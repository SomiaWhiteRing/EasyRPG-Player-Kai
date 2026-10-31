package org.easyrpg.player.button_mapping;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ScrollView;

import androidx.annotation.NonNull;
import androidx.core.view.GravityCompat;
import androidx.drawerlayout.widget.DrawerLayout;

import com.google.android.material.navigation.NavigationView;

import org.easyrpg.player.BaseActivity;
import org.easyrpg.player.Helper;
import org.easyrpg.player.R;
import org.easyrpg.player.settings.SettingsManager;

import java.util.LinkedList;
import java.util.List;

public class ButtonMappingActivity extends BaseActivity implements NavigationView.OnNavigationItemSelectedListener {
    private DrawerLayout drawer;
    private ViewGroup layoutManager;
    private List<VirtualButton> buttonList;
    private InputLayout inputLayout;
    private InputLayout.Orientation orientation;

    public static final String TAG_ORIENTATION = "orientation";
    public static final int TAG_ORIENTATION_VALUE_HORIZONTAL = 0,TAG_ORIENTATION_VALUE_VERTICAL = 1;

    public static boolean samsungMultitouchWorkaround = false;
    public static int pointerCount = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.button_mapping_activity);

        SettingsManager.init(getApplicationContext());

        // Menu configuration
        this.drawer = findViewById(R.id.drawer_layout);
        drawer.setDrawerLockMode(DrawerLayout.LOCK_MODE_LOCKED_CLOSED);
        NavigationView navigationView = findViewById(R.id.nav_view);
        navigationView.setNavigationItemSelectedListener(this);

        // Hide the status bar
        hideStatusBar();

        // Setup the activity depending on the orientation of the inputLayout to modify
        layoutManager = findViewById(R.id.button_mapping_activity_layout);
        if (getIntent().getIntExtra(TAG_ORIENTATION, TAG_ORIENTATION_VALUE_HORIZONTAL) == TAG_ORIENTATION_VALUE_HORIZONTAL) {
            this.inputLayout = SettingsManager.getInputLayoutHorizontal(this);
            this.orientation = InputLayout.Orientation.ORIENTATION_HORIZONTAL;
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        } else {
            this.inputLayout = SettingsManager.getInputLayoutVertical(this);
            this.orientation = InputLayout.Orientation.ORIENTATION_VERTICAL;
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
        }

        // Copy the inputLayout's buttons (to be able to reset later)
        buttonList = new LinkedList<>();
        VirtualButton vb = null;
        for (VirtualButton b : inputLayout.getButtonList()) {
            if (b instanceof VirtualCross) {
                vb = new VirtualCross(this, b.getPosX(), b.getPosY(), b.getSize());
            } else if (b.keyCode > 0) {
                vb = VirtualButton.Create(this, b.getKeyCode(), b.getPosX(), b.getPosY(), b.getSize());
            } else if (b.keyCode == MenuButton.MENU_BUTTON_KEY) {
                vb = new MenuButton(this, b.getPosX(), b.getPosY(), b.getSize());
            }
            buttonList.add(vb);
        }

        if (Build.VERSION.SDK_INT >= 35) {
            getWindow().getDecorView().setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @NonNull
                @Override
                public WindowInsets onApplyWindowInsets(@NonNull View view, @NonNull WindowInsets insets) {
                    drawButtons();
                    return insets;
                }
            });
        } else {
            drawButtons();
        }

        onBackPressedCallback.setEnabled(true);
    }

    @Override
    protected void onResume() {
        super.onResume();

        // Retrieve User's preferences
        SettingsManager.init(getApplicationContext());
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        pointerCount = ev.getPointerCount();
        return super.dispatchTouchEvent(ev);
    }

    /**
     * Called after a screen orientation changement
     */
    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);

        // We draw the button again to match the positions
        drawButtons();
    }

    @Override
    public boolean onNavigationItemSelected(MenuItem item) {
        if (item.getItemId() == R.id.button_mapping_menu_add_button) {
            showSupportedButton();
            openOrCloseMenu();
        } else if (item.getItemId() == R.id.button_mapping_menu_add_custom_button) {
            showCustomKeyboard();
            openOrCloseMenu();
        } else if (item.getItemId() == R.id.button_mapping_menu_reset) {
            if (orientation == InputLayout.Orientation.ORIENTATION_HORIZONTAL) {
                buttonList = InputLayout.getDefaultInputLayoutHorizontal(this).getButtonList();
            } else {
                buttonList = InputLayout.getDefaultInputLayoutVertical(this).getButtonList();
            }
            drawButtons();
            openOrCloseMenu();
        } else if (item.getItemId() == R.id.button_mapping_menu_exit_without_saving) {
            this.finish();
        } else if (item.getItemId() == R.id.button_mapping_menu_save_and_quit) {
            save();
            this.finish();
        } else {
            return false;
        }
        return true;
    }

    public void backPressed() {
        openOrCloseMenu();
    }

    public void openOrCloseMenu() {
        if (drawer.isDrawerOpen(GravityCompat.START)) {
            drawer.closeDrawer(GravityCompat.START);
        } else {
            drawer.openDrawer(GravityCompat.START);
        }
    }

    public void hideStatusBar() {
        // Hide the status bar
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
    }

    public void save() {
        //Copy the button from layoutList to the InputLayout
        inputLayout.getButtonList().clear();
        for (VirtualButton b : buttonList) {
            if (b instanceof VirtualCross) {
                inputLayout.getButtonList().add(new VirtualCross(this, b.getPosX(), b.getPosY(), b.getSize()));
            }
            else if (b.keyCode > 0) {
                inputLayout.getButtonList().add(VirtualButton.Create(this, b.getKeyCode(), b.getPosX(), b.getPosY(), b.getSize()));
            } else if (b.keyCode == MenuButton.MENU_BUTTON_KEY) {
                inputLayout.getButtonList().add(new MenuButton(this, b.getPosX(), b.getPosY(), b.getSize()));
            }
        }

        //Save the ButtonMappingModel
        if (orientation == InputLayout.Orientation.ORIENTATION_HORIZONTAL) {
            SettingsManager.setInputLayoutHorizontal(this, inputLayout);
        } else {
            SettingsManager.setInputLayoutVertical(this, inputLayout);
        }
    }

    public void showSupportedButton() {
        Context ctx = getApplicationContext();
        final CharSequence[] items = {
                ctx.getString(R.string.key_enter),
                ctx.getString(R.string.key_cancel),
                ctx.getString(R.string.key_shift),
                ctx.getString(R.string.menu),
                ctx.getString(R.string.key_fast_forward),
                ctx.getString(R.string.key_debug_menu),
                ctx.getString(R.string.key_debug_walk_through_walls),
                ctx.getString(R.string.key_history),
                ctx.getString(R.string.key_highlight)
        };
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle(getResources().getString(R.string.add_a_button));
        builder.setItems(items, (dialog, item) -> {
            //Toast.makeText(getApplicationContext(), items[item], Toast.LENGTH_SHORT).show();
            addAButton(items[item].toString());
        });
        AlertDialog alert = builder.create();
        alert.show();
    }

    private void showCustomKeyboard() {
        String[] rows = {
                "ESCAPE _:1 F1 F2 F3 F4 _:0.5 F5 F6 F7 F8 _:0.5 F9 F10 F11 F12",
                "GRAVE 1 2 3 4 5 6 7 8 9 0 MINUS EQUALS DEL:2 _:0.5 INSERT HOME PAGE_UP _:0.5 NUM_LOCK NUMPAD_DIVIDE NUMPAD_MULTIPLY NUMPAD_SUBTRACT",
                "TAB:1.5 Q W E R T Y U I O P LEFT_BRACKET RIGHT_BRACKET BACKSLASH:1.5 _:0.5 FORWARD_DEL MOVE_END PAGE_DOWN _:0.5 NUMPAD_7 NUMPAD_8 NUMPAD_9 NUMPAD_ADD",
                "CAPS_LOCK:1.75 A S D F G H J K L SEMICOLON APOSTROPHE ENTER:2.25 _:4 NUMPAD_4 NUMPAD_5 NUMPAD_6 _:1",
                "SHIFT_LEFT:2.25 Z X C V B N M COMMA PERIOD SLASH SHIFT_RIGHT:2.75 _:1.5 DPAD_UP _:1.5 NUMPAD_1 NUMPAD_2 NUMPAD_3 NUMPAD_ENTER",
                "CTRL_LEFT:1.25 _:1.25 ALT_LEFT:1.25 SPACE:6.25 ALT_RIGHT:1.25 _:1.25 _:1.25 CTRL_RIGHT:1.25 _:0.5 DPAD_LEFT DPAD_DOWN DPAD_RIGHT _:0.5 NUMPAD_0:2 NUMPAD_DOT"
        };
        int[] reserved = {
                KeyEvent.KEYCODE_ESCAPE, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_SPACE,
                KeyEvent.KEYCODE_Z, KeyEvent.KEYCODE_X, KeyEvent.KEYCODE_B,
                KeyEvent.KEYCODE_E, KeyEvent.KEYCODE_F, KeyEvent.KEYCODE_GRAVE,
                KeyEvent.KEYCODE_SHIFT_LEFT, KeyEvent.KEYCODE_SHIFT_RIGHT,
                KeyEvent.KEYCODE_CTRL_LEFT, KeyEvent.KEYCODE_CTRL_RIGHT,
                KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN,
                KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT,
                KeyEvent.KEYCODE_F1, KeyEvent.KEYCODE_F2, KeyEvent.KEYCODE_F3,
                KeyEvent.KEYCODE_F4, KeyEvent.KEYCODE_F5, KeyEvent.KEYCODE_F7,
                KeyEvent.KEYCODE_F9, KeyEvent.KEYCODE_F10, KeyEvent.KEYCODE_F11, KeyEvent.KEYCODE_F12,
                KeyEvent.KEYCODE_NUMPAD_ENTER
        };
        int keyWidth = (int) (56 * getResources().getDisplayMetrics().density);
        int keyHeight = (int) (52 * getResources().getDisplayMetrics().density);
        FrameLayout keyboard = new FrameLayout(this);
        HorizontalScrollView horizontal = new HorizontalScrollView(this);
        horizontal.addView(keyboard, new ViewGroup.LayoutParams(keyWidth * 23, keyHeight * rows.length));
        ScrollView vertical = new ScrollView(this);
        vertical.addView(horizontal);
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle(R.string.add_custom_button)
                .setView(vertical)
                .create();
        for (int rowIndex = 0; rowIndex < rows.length; rowIndex++) {
            int x = 0;
            for (String entry : rows[rowIndex].split(" ")) {
                String[] parts = entry.split(":");
                String name = parts[0];
                int width = parts.length == 2 ? (int) (keyWidth * Float.parseFloat(parts[1])) : keyWidth;
                if (name.equals("_")) {
                    x += width;
                    continue;
                }
                int code = KeyEvent.keyCodeFromString("KEYCODE_" + name);
                Button key = new Button(this);
                key.setText(VirtualButton.customKeyLabel(code).replace("Num ", ""));
                key.setTextSize(12);
                key.setAllCaps(false);
                key.setPadding(0, 0, 0, 0);
                key.setMinWidth(0);
                key.setMinimumWidth(0);
                boolean used = code == KeyEvent.KEYCODE_UNKNOWN;
                for (int existing : reserved) {
                    used |= code == existing;
                }
                for (VirtualButton existing : buttonList) {
                    int existingCode = existing.getKeyCode();
                    if (existingCode >= VirtualButton.CUSTOM_KEY_OFFSET) {
                        existingCode -= VirtualButton.CUSTOM_KEY_OFFSET;
                    }
                    used |= existingCode == code;
                    if (code >= KeyEvent.KEYCODE_0 && code <= KeyEvent.KEYCODE_9) {
                        used |= existingCode == KeyEvent.KEYCODE_NUMPAD_0 + code - KeyEvent.KEYCODE_0;
                    } else if (code >= KeyEvent.KEYCODE_NUMPAD_0 && code <= KeyEvent.KEYCODE_NUMPAD_9) {
                        used |= existingCode == KeyEvent.KEYCODE_0 + code - KeyEvent.KEYCODE_NUMPAD_0;
                    }
                    if (!SettingsManager.getShowABasZX() && (code == KeyEvent.KEYCODE_A || code == KeyEvent.KEYCODE_B)) {
                        used |= existing.getKeyCode() == (code == KeyEvent.KEYCODE_A ? VirtualButton.ENTER : VirtualButton.CANCEL);
                    }
                }
                key.setEnabled(!used);
                key.setOnClickListener(view -> {
                    buttonList.add(VirtualButton.Create(this, VirtualButton.CUSTOM_KEY_OFFSET + code, 0.5, 0.5, 100));
                    drawButtons();
                    dialog.dismiss();
                });
                int height = code == KeyEvent.KEYCODE_NUMPAD_ADD || code == KeyEvent.KEYCODE_NUMPAD_ENTER ? keyHeight * 2 : keyHeight;
                FrameLayout.LayoutParams position = new FrameLayout.LayoutParams(width, height);
                position.leftMargin = x;
                position.topMargin = rowIndex * keyHeight;
                keyboard.addView(key, position);
                x += width;
            }
        }
        dialog.show();
    }

    public void addAButton(String s) {
        int keyCode = -1;
        Context ctx = getApplicationContext();

        if (s.equals(ctx.getString(R.string.key_enter))) {
            keyCode = KeyEvent.KEYCODE_SPACE;
        } else if (s.equals(ctx.getString(R.string.key_cancel))) {
            keyCode = KeyEvent.KEYCODE_B;
        } else if (s.equals(ctx.getString(R.string.key_shift))) {
            keyCode = KeyEvent.KEYCODE_SHIFT_LEFT;
        } else if (s.equals(ctx.getString(R.string.menu))) {
            keyCode = MenuButton.MENU_BUTTON_KEY;
        } else if (s.equals(ctx.getString(R.string.key_fast_forward))) {
            keyCode = KeyEvent.KEYCODE_F;
        } else if (s.equals(ctx.getString(R.string.key_debug_menu))) {
            keyCode = KeyEvent.KEYCODE_F9;
        } else if (s.equals(ctx.getString(R.string.key_debug_walk_through_walls))) {
            keyCode = KeyEvent.KEYCODE_CTRL_LEFT;
        } else if (s.equals(ctx.getString(R.string.key_history))) {
            keyCode = VirtualButton.KEY_HISTORY;
        } else if (s.equals(ctx.getString(R.string.key_highlight))) {
            keyCode = VirtualButton.KEY_HIGHLIGHT;
        }

        VirtualButton vb = null;
        if (keyCode > 0) {
            vb = VirtualButton.Create(this, keyCode, 0.5, 0.5, 100);
        } else if (keyCode == MenuButton.MENU_BUTTON_KEY){
            vb = new MenuButton(this, 0.5, 0.5, 100);
        }
        buttonList.add(vb);
        drawButtons();
    }

    /**
     * Draws all buttons.
     */
    private void drawButtons() {
        layoutManager.removeAllViews();
        for (VirtualButton b : buttonList) {
            b.setDebug_mode(true);
            layoutManager.addView(b);
            Helper.setLayoutPosition(this, b, b.getPosX(), b.getPosY());
        }
    }

    public static void dragVirtualButton(VirtualButton v, MotionEvent event) {
        float x, y;

        int action = event.getActionMasked();

        switch (action) {
            case (MotionEvent.ACTION_DOWN):
                // Samsung reports incorrect coordinates when a multitouch across multiple views
                // happens while a game is running. When the bug happens then a touch event will
                // report out-of-bounds coordinates for the initial touch. We use this to detect
                // the bug (see #2915)
                if (pointerCount > 1 && (event.getX() < 0 || event.getY() < 0)) {
                    Log.i("EasyRPG", "Applying Samsung Touch Workaround");
                    samsungMultitouchWorkaround = true;
                }
                // fallthrough
            case (MotionEvent.ACTION_MOVE):
                // Calculation of the new view position
                ViewGroup parentVg = (ViewGroup)v.getParent();

                x = (v.getLeft() + event.getX() - v.getWidth() / 2f)
                        / parentVg.getMeasuredWidth();
                y = (v.getTop() + event.getY() - v.getHeight() / 2f)
                        / parentVg.getMeasuredHeight();

                if (pointerCount > 1 && samsungMultitouchWorkaround) {
                    double scale = Helper.getTouchScale(v.getContext());
                    x /= scale;
                    y /= scale;
                }

                // Rounding (x,y) in order to place the buttons on a grid
                x = Math.round(x*100f) / 100f;
                y = Math.round(y*100f) / 100f;

                Helper.setLayoutPosition((Activity) v.getContext(), v, x, y);

                v.setPosX(x);
                v.setPosY(y);

                return;
            default:
        }
    }
}
