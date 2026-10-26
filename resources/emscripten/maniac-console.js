// Direct browser host for Maniac console commands.
(() => {
  const canvas = Module.canvas;
  const consolePalette = ['#000000', '#000080', '#008000', '#008080', '#800000', '#800080', '#808000', '#c0c0c0',
    '#808080', '#0000ff', '#00ff00', '#00ffff', '#ff0000', '#ff00ff', '#ffff00', '#ffffff'];
  let consoleWindow, consoleInput;
  let consoleOwned = false, consoleGeneration = 0;
  let foreground = 7, background = 0;

  function closeConsoleWindow(notify = false) {
    if (notify && consoleOwned) {
      consoleOwned = false;
      ++consoleGeneration;
    }
    if (consoleInput) consoleInput(null);
    const panel = consoleWindow;
    consoleWindow = undefined;
    if (panel) panel.dialog.remove();
    if (canvas) canvas.focus();
  }

  function appendConsoleText(text, foreground, background) {
    const panel = consoleWindow;
    if (!panel || !text) return;
    const font = getComputedStyle(panel.output);
    panel.measure.font = `${font.fontStyle} ${font.fontWeight} ${font.fontSize} ${font.fontFamily}`;
    const space = panel.measure.measureText(' ').width;
    panel.columns ||= Math.max(1, Math.floor(panel.output.clientWidth / space));
    const color = `${foreground}:${background}`;
    const styled = value => {
      const run = document.createElement('span'); run.dataset.color = color;
      run.style.color = consolePalette[foreground]; run.style.backgroundColor = consolePalette[background];
      run.append(document.createTextNode(value)); return run;
    };
    const glyphColumns = glyph => Math.max(1, Math.round(panel.measure.measureText(glyph).width / panel.measure.measureText(' ').width));
    const writeRun = value => {
      const before = panel.row.textContent;
      let width = 0;
      for (const item of panel.segmenter ? panel.segmenter.segment(value) : value) width += glyphColumns(item.segment ?? item);
      let start = before.length, end = before.length, firstColumn = 0, lastColumn = 0, offset = 0, column = 0;
      for (const item of panel.segmenter ? panel.segmenter.segment(before) : before) {
        const glyph = item.segment ?? item, next = column + glyphColumns(glyph);
        if (next > panel.column && column < panel.column + width) {
          if (start === before.length) { start = offset; firstColumn = column; }
          end = offset + glyph.length; lastColumn = next;
        }
        offset += glyph.length; column = next;
      }
      if (start === before.length) firstColumn = lastColumn = column;
      const left = Math.max(0, panel.column - firstColumn), right = Math.max(0, lastColumn - panel.column - width);
      if (start === before.length && !left) {
        const last = panel.row.lastElementChild;
        if (last?.dataset.color === color) last.firstChild.appendData(value);
        else panel.row.append(styled(value));
      } else {
        const oldRuns = [...panel.row.children];
        // Replacing half of a wide glyph leaves the other cell blank, with its old colors.
        const padding = (count, index) => {
          let at = 0, old;
          for (const run of oldRuns) {
            if (index >= at && index < at + run.textContent.length) { old = run; break; }
            at += run.textContent.length;
          }
          const span = old ? old.cloneNode(false) : document.createElement('span');
          if (!old) { span.dataset.color = '7:0'; span.style.color = consolePalette[7]; span.style.backgroundColor = consolePalette[0]; }
          span.append(document.createTextNode(' '.repeat(count))); return span;
        };
        const fragment = document.createDocumentFragment();
        let offset = 0, inserted = false;
        const insert = () => {
          if (left) fragment.append(padding(left, start));
          fragment.append(styled(value));
          if (right) fragment.append(padding(right, end - 1));
          inserted = true;
        };
        for (const run of oldRuns) {
          const valueBefore = run.textContent, next = offset + valueBefore.length;
          const copy = part => {
            if (!part) return;
            const span = run.cloneNode(false); span.append(document.createTextNode(part)); fragment.append(span);
          };
          if (next <= start) fragment.append(run);
          else {
            copy(valueBefore.slice(0, Math.max(0, start - offset)));
            if (!inserted) insert();
            copy(valueBefore.slice(Math.max(0, end - offset)));
          }
          offset = next;
        }
        if (!inserted) insert();
        panel.row.replaceChildren(fragment);
      }
      panel.cursor = start + left + value.length; panel.column += width;
      panel.rowLength = panel.row.textContent.length;
    };
    const nextRow = () => {
      panel.output.append(document.createTextNode('\n'));
      panel.row = document.createElement('span'); panel.output.append(panel.row);
      panel.cursor = panel.rowLength = panel.column = 0;
    };
    const write = value => {
      const width = panel.columns;
      let used = panel.column;
      let run = '';
      const glyphs = panel.segmenter ? panel.segmenter.segment(value) : value;
      for (const item of glyphs) {
        const glyph = item.segment ?? item;
        const advance = glyphColumns(glyph);
        // Filling the last column delays wrapping until the next printable glyph.
        if (used > 0 && used + advance > width + 0.01) {
          if (run) writeRun(run);
          nextRow(); run = ''; used = 0;
        }
        run += glyph; used += advance;
      }
      if (run) writeRun(run);
    };
    let bell = false;
    for (let i = 0; i < text.length;) {
      const character = text[i++];
      if (character === '\r') panel.cursor = panel.column = 0;
      else if (character === '\b' && panel.column) {
        const before = panel.row.textContent;
        let column = 0, offset = 0, found = false;
        for (const item of panel.segmenter ? panel.segmenter.segment(before) : before) {
          const glyph = item.segment ?? item, next = column + glyphColumns(glyph);
          if (next >= panel.column) { panel.column = column; panel.cursor = offset; found = true; break; }
          column = next; offset += glyph.length;
        }
        if (!found) { --panel.column; panel.cursor = before.length; }
      }
      else if (character === '\n') {
        nextRow();
      } else if (character === '\t') {
        // Legacy WriteConsoleW advances to a tab stop without erasing existing cells.
        panel.column += Math.min(panel.columns - panel.column, 8 - (panel.column & 7));
        panel.cursor = panel.row.textContent.length;
      } else if (character === '\x07') bell = true;
      else if (character === '\b') { /* Already at the row start. */ }
      else if (character === '\0') write(' ');
      else {
        const start = i - 1;
        while (i < text.length) {
          const code = text.charCodeAt(i);
          if (code === 0 || (code >= 7 && code <= 10) || code === 13) break;
          ++i;
        }
        write(text.slice(start, i));
      }
    }
    // A page has no OS console bell; coalesce this write into a visual alert.
    if (bell) panel.dialog.animate([{opacity: 0.5}, {opacity: 1}], {duration: 150});
    panel.output.scrollTop = panel.output.scrollHeight;
  }
  function setConsoleColor(foreground, background) {
    const panel = consoleWindow;
    panel.foreground = foreground; panel.background = background;
    panel.input.style.color = consolePalette[foreground];
    panel.input.style.backgroundColor = consolePalette[background];
  }
  function openConsoleWindow(data) {
    if (consoleWindow) { consoleWindow.caption.textContent = data.text; return; }
    const gameActive = document.activeElement === canvas || document.activeElement === document.body;
    const dialog = document.createElement('dialog');
    dialog.setAttribute('aria-label', '控制台'); dialog.tabIndex = -1;
    Object.assign(dialog.style, {position: 'fixed', inset: '16px 16px auto auto', margin: '0',
      width: 'min(640px, calc(100vw - 32px))', height: 'min(480px, calc(100vh - 32px))',
      boxSizing: 'border-box', padding: '12px', zIndex: '1000', background: '#000', color: '#c0c0c0'});
    const header = document.createElement('div'); header.style.display = 'flex'; header.style.gap = '8px';
    const caption = document.createElement('strong'); caption.textContent = data.text; caption.style.flex = '1';
    const close = document.createElement('button'); close.type = 'button'; close.textContent = '关闭';
    close.onclick = () => closeConsoleWindow(true);
    header.append(caption, close);
    const output = document.createElement('pre');
    Object.assign(output.style, {height: 'calc(100% - 64px)', overflow: 'auto', whiteSpace: 'pre', margin: '8px 0'});
    output.tabIndex = 0; output.setAttribute('aria-label', '控制台输出');
    const form = document.createElement('form'); form.hidden = true;
    const input = document.createElement('input'); input.type = 'text'; input.setAttribute('aria-label', '控制台输入');
    input.style.width = '60%';
    const submit = document.createElement('button'); submit.type = 'submit'; submit.textContent = '确定';
    const cancel = document.createElement('button'); cancel.type = 'button'; cancel.textContent = '取消';
    form.append(input, submit, cancel); dialog.append(header, output, form); document.body.append(dialog);
    const row = document.createElement('span'); output.append(row);
    consoleWindow = {dialog, caption, output, form, input, cancel, generation: data.generation,
      row, cursor: 0, rowLength: 0, column: 0,
      segmenter: typeof Intl.Segmenter === 'function' ? new Intl.Segmenter(undefined, {granularity: 'grapheme'}) : null,
      measure: document.createElement('canvas').getContext('2d')};
    setConsoleColor(data.foreground, data.background);
    dialog.onclose = () => { if (consoleWindow?.dialog === dialog) closeConsoleWindow(true); };
    try { dialog.show(); }
    catch (error) { closeConsoleWindow(true); throw error; }
    if (gameActive) canvas.focus();
  }
  Module.consoleCommand = command => {
    if (command.op === 'close') {
      if (!consoleOwned) return true;
      consoleOwned = false;
      ++consoleGeneration;
      closeConsoleWindow();
      return true;
    }
    if (command.op === 'open') {
      if (consoleOwned && !command.options) return true;
      if (!consoleOwned) {
        consoleOwned = true;
        ++consoleGeneration;
        foreground = 7; background = 0;
      }
      try {
        openConsoleWindow({...command, generation: consoleGeneration, foreground, background});
        return true;
      } catch (error) {
        closeConsoleWindow(true);
        console.error('Could not open Maniac console:', error);
        return false;
      }
    }
    if (!consoleOwned) return false;
    if (command.op === 'write') appendConsoleText(command.text, foreground, background);
    else if (command.op === 'color') {
      if (command.options & 8) foreground = command.foreground & 15;
      if (command.options & 16) background = command.background & 15;
      setConsoleColor(foreground, background);
    } else if (command.op === 'focus') {
      const inConsole = consoleWindow.dialog.contains(document.activeElement);
      const inGame = document.activeElement === canvas || document.activeElement === document.body;
      if (command.options && inGame) consoleWindow.dialog.focus();
      else if (!command.options && inConsole) canvas.focus();
    }
    return true;
  };

  Module.getConsoleGeneration = () => consoleGeneration;
  Module.readConsoleLine = title => {
    if (!consoleOwned || consoleInput) return Promise.resolve(null);
    return new Promise(resolve => {
      const panel = consoleWindow;
      const {dialog, form, caption, input, cancel} = panel;
      caption.textContent = title;
      input.value = '';
      form.hidden = false;
      const escape = event => {
        if (event.key === 'Escape') { event.preventDefault(); finish(null); }
      };
      const finish = value => {
        if (!consoleInput) return;
        consoleInput = undefined;
        dialog.removeEventListener('keydown', escape);
        form.onsubmit = null; cancel.onclick = null; form.hidden = true;
        if (typeof value === 'string') appendConsoleText(value + '\r\n', foreground, background);
        canvas.focus();
        resolve(value);
      };
      consoleInput = finish;
      dialog.addEventListener('keydown', escape);
      form.onsubmit = event => { event.preventDefault(); finish(input.value); };
      cancel.onclick = () => finish(null);
      input.focus();
    });
  };

  Module.confirmFileOutput = () => Promise.resolve(window.confirm(
    'Outputting multiple files in a short time. Continue?'));
})();
