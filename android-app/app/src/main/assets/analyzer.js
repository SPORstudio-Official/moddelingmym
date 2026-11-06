/*
 * Code Reviewer - rule-based static analyzer.
 *
 * This file contains NO generative AI and NO network calls. Every finding comes
 * from a deterministic rule (bracket matching, string/comment lexing, regular
 * expressions and simple structural checks) with a fixed recommendation.
 *
 * Works in two places:
 *   - inside the Android WebView (loaded as a plain <script>, exposes window.CodeAnalyzer)
 *   - in Node.js for unit tests (module.exports)
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.CodeAnalyzer = factory();
  }
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var SEVERITY = { error: 3, warning: 2, info: 1 };

  // ---------------------------------------------------------------------------
  // Language detection
  // ---------------------------------------------------------------------------
  var LANGUAGES = ['javascript', 'python', 'java', 'c', 'json', 'html'];

  function detectLanguage(code) {
    var src = code || '';
    var trimmed = src.trim();
    if (!trimmed) return 'javascript';

    if (isValidJson(trimmed)) return 'json';
    if (/^\{\s*("|\}$)/.test(trimmed) || /^\[\s*(["{\[\d\-]|\]$)/.test(trimmed) && !/[;=]/.test(trimmed)) return 'json';
    if (/^\s*(<!doctype html|<html|<div|<body|<head)/i.test(trimmed)) return 'html';

    var scores = { javascript: 0, python: 0, java: 0, c: 0 };
    if (/#include\s*[<"]/.test(src) || /\bint\s+main\s*\(/.test(src) || /\bprintf\s*\(/.test(src)) scores.c += 3;
    if (/\bpublic\s+(static\s+)?(class|void|final)\b|\bSystem\.out\.|\bString\[\]\s*args/.test(src)) scores.java += 3;
    if (/^\s*def\s+\w+\s*\(.*\)\s*:/m.test(src) || /^\s*(elif\s.*:|import\s+\w+$)/m.test(src)) scores.python += 3;
    if (/\bprint\s*\(/.test(src) && !/;\s*$/m.test(src)) scores.python += 1;
    if (/\b(const|let|var)\s+\w+/.test(src) || /=>/.test(src) || /console\.log/.test(src)) scores.javascript += 2;
    if (/;\s*$/m.test(src)) { scores.javascript += 1; scores.java += 1; scores.c += 1; }
    if (/^\s*function\s+\w+/m.test(src)) scores.javascript += 2;

    var best = 'javascript', bestScore = -1;
    Object.keys(scores).forEach(function (k) {
      if (scores[k] > bestScore) { best = k; bestScore = scores[k]; }
    });
    return best;
  }

  function isValidJson(text) {
    try { JSON.parse(text); return true; } catch (e) { return false; }
  }

  // ---------------------------------------------------------------------------
  // Lexing: blank out comments and string contents so rules do not fire inside
  // them. Newlines are preserved so line numbers stay correct.
  // ---------------------------------------------------------------------------
  function blankOut(text) {
    return text.replace(/[^\n]/g, ' ');
  }

  function stripCodeForRules(code, language) {
    var out = '';
    var i = 0, n = code.length;
    var unclosed = [];
    var lang = language;
    var hasHashComments = lang === 'python';
    var hasSlashComments = lang !== 'python' && lang !== 'json';
    var hasBlockComments = lang === 'javascript' || lang === 'java' || lang === 'c';

    // Track position of each unclosed string so we can report it.
    while (i < n) {
      var ch = code[i];
      var next = code[i + 1];
      var line = lineOf(code, i);

      // Python triple-quoted strings (docstrings) - blank the whole thing.
      if (lang === 'python' && (code.substr(i, 3) === '"""' || code.substr(i, 3) === "'''")) {
        var q3 = code.substr(i, 3);
        var end3 = code.indexOf(q3, i + 3);
        if (end3 === -1) {
          unclosed.push({ line: line, kind: 'string' });
          out += blankOut(code.slice(i));
          break;
        }
        out += blankOut(code.slice(i, end3 + 3));
        i = end3 + 3;
        continue;
      }

      // Block comments
      if (hasBlockComments && ch === '/' && next === '*') {
        var endC = code.indexOf('*/', i + 2);
        if (endC === -1) {
          unclosed.push({ line: line, kind: 'comment' });
          out += blankOut(code.slice(i));
          break;
        }
        out += blankOut(code.slice(i, endC + 2));
        i = endC + 2;
        continue;
      }

      // Line comments
      if ((hasSlashComments && ch === '/' && next === '/') || (hasHashComments && ch === '#')) {
        var eol = code.indexOf('\n', i);
        if (eol === -1) eol = n;
        out += blankOut(code.slice(i, eol));
        i = eol;
        continue;
      }

      // String literals
      if (ch === '"' || ch === "'" || (lang === 'javascript' && ch === '`')) {
        var j = i + 1;
        var closed = false;
        while (j < n && code[j] !== '\n') {
          if (code[j] === '\\') { j += 2; continue; }
          if (code[j] === ch) { closed = true; break; }
          j++;
        }
        if (!closed) {
          // Template literals may span lines; everything else must close on the same line.
          if (lang === 'javascript' && ch === '`') {
            var endT = code.indexOf('`', i + 1);
            if (endT !== -1) {
              out += blankOut(code.slice(i, endT + 1));
              i = endT + 1;
              continue;
            }
          }
          unclosed.push({ line: line, kind: 'string' });
          out += blankOut(code.slice(i, j));
          i = j;
          continue;
        }
        out += ch + blankOut(code.slice(i + 1, j)) + ch;
        i = j + 1;
        continue;
      }

      out += ch;
      i++;
    }
    return { text: out, unclosed: unclosed };
  }

  function lineOf(code, index) {
    var line = 1;
    for (var k = 0; k < index && k < code.length; k++) if (code[k] === '\n') line++;
    return line;
  }

  // Returns an array of line strings from the stripped text.
  function splitLines(text) {
    return text.split(/\r?\n/);
  }

  // ---------------------------------------------------------------------------
  // Issue helper
  // ---------------------------------------------------------------------------
  function makeIssue(severity, category, line, message, recommendation, rawLines) {
    var snippet = rawLines && rawLines[line - 1] !== undefined ? rawLines[line - 1].trim() : '';
    if (snippet.length > 140) snippet = snippet.slice(0, 137) + '...';
    return {
      severity: severity,
      category: category,
      line: line,
      message: message,
      recommendation: recommendation,
      snippet: snippet
    };
  }

  // ---------------------------------------------------------------------------
  // Generic checks (apply to every language)
  // ---------------------------------------------------------------------------
  function genericChecks(ctx) {
    var issues = [];
    var raw = ctx.rawLines;

    // Brackets: ( ) [ ] { }
    var pairs = { ')': '(', ']': '[', '}': '{' };
    var stack = [];
    var stripped = ctx.stripped;
    var lineNo = 1;
    var mismatchReported = false;
    for (var i = 0; i < stripped.length; i++) {
      var c = stripped[i];
      if (c === '\n') { lineNo++; continue; }
      if (c === '(' || c === '[' || c === '{') {
        stack.push({ ch: c, line: lineNo });
      } else if (c === ')' || c === ']' || c === '}') {
        if (!stack.length) {
          issues.push(makeIssue('error', 'Syntax', lineNo,
            'Unexpected closing "' + c + '" with no matching opening bracket.',
            'Remove the extra "' + c + '" or add the missing opening bracket earlier.', raw));
        } else if (stack[stack.length - 1].ch !== pairs[c]) {
          if (!mismatchReported) {
            var open = stack[stack.length - 1];
            issues.push(makeIssue('error', 'Syntax', lineNo,
              'Mismatched bracket: "' + c + '" closes "' + open.ch + '" opened on line ' + open.line + '.',
              'Check the nesting order of brackets. Each closing bracket must match the most recent open one.', raw));
            mismatchReported = true;
          }
          stack.pop();
        } else {
          stack.pop();
        }
      }
    }
    stack.forEach(function (open) {
      issues.push(makeIssue('error', 'Syntax', open.line,
        'Opening "' + open.ch + '" is never closed.',
        'Add the matching closing bracket, or check whether an earlier block ended too early.', raw));
    });

    ctx.unclosed.forEach(function (u) {
      if (u.kind === 'comment') {
        issues.push(makeIssue('error', 'Syntax', u.line,
          'Block comment starts here but is never closed.',
          'Add the closing marker for the block comment.', raw));
      } else {
        issues.push(makeIssue('error', 'Syntax', u.line,
          'String literal is not closed on this line.',
          'Add the matching quote character, or escape it with a backslash if it is part of the text.', raw));
      }
    });

    // Per-line checks
    var strippedLines = splitLines(stripped);
    var sawTabIndent = false, sawSpaceIndent = false;
    var todoCount = 0;
    raw.forEach(function (line, idx) {
      var ln = idx + 1;
      if (/\s+$/.test(line) && line.trim().length > 0) {
        issues.push(makeIssue('info', 'Style', ln,
          'Trailing whitespace at end of line.',
          'Remove trailing spaces. Most editors can do this automatically on save.', raw));
      }
      if (line.length > 120) {
        issues.push(makeIssue('info', 'Style', ln,
          'Line is ' + line.length + ' characters long.',
          'Split long lines into shorter statements (aim for under 120 characters) for readability.', raw));
      }
      if (/\t/.test(line.match(/^\s*/)[0])) sawTabIndent = true;
      if (/^ +\S/.test(line)) sawSpaceIndent = true;

      var comment = line.match(/(TODO|FIXME|HACK|XXX)\b/);
      if (comment) {
        todoCount++;
        if (todoCount <= 10) {
          issues.push(makeIssue('info', 'Maintenance', ln,
            comment[1] + ' marker found.',
            'Resolve the note or move it to your task list before shipping.', raw));
        }
      }

      var secret = /\b(password|passwd|pwd|secret|api[_-]?key|access[_-]?token|auth[_-]?token|private[_-]?key)\b\s*[:=]\s*(["'`])([^"'`]{4,})\2/i.exec(line);
      if (secret && !/\b(example|placeholder|changeme|your[_-])/i.test(secret[3])) {
        issues.push(makeIssue('error', 'Security', ln,
          'Possible hard-coded credential: "' + secret[1] + '" is assigned a literal value.',
          'Load secrets from environment variables or a secure secret store instead of committing them to source.', raw));
      }
    });

    if (sawTabIndent && sawSpaceIndent) {
      issues.push(makeIssue('warning', 'Style', 1,
        'Indentation mixes tabs and spaces.',
        'Use one indentation style consistently (spaces are the most common choice).', raw));
    }

    if (ctx.code.trim().length === 0) {
      issues.push(makeIssue('info', 'General', 1,
        'The pasted code is empty.',
        'Paste some code to review.', raw));
    }
    return issues;
  }

  // ---------------------------------------------------------------------------
  // JavaScript rules
  // ---------------------------------------------------------------------------
  function javascriptChecks(ctx) {
    var issues = [];
    var raw = ctx.rawLines;
    var lines = splitLines(ctx.stripped);
    lines.forEach(function (s, idx) {
      var ln = idx + 1;
      var r = function (sev, cat, msg, rec) { issues.push(makeIssue(sev, cat, ln, msg, rec, raw)); };
      if (/[^=!<>]==[^=]|!=[^=]/.test(s)) {
        r('warning', 'Logic', 'Loose equality (== or !=) can coerce types unexpectedly.',
          'Use strict equality (=== or !==) unless you intentionally want type coercion.');
      }
      if (/\bif\s*\(\s*[\w.\[\]]+\s*=[^=>]/.test(s)) {
        r('error', 'Logic', 'Assignment (=) used inside an if condition.',
          'If you meant to compare, use === . If the assignment is intentional, wrap it in extra parentheses and explain it in a comment.');
      }
      if (/\beval\s*\(/.test(s)) {
        r('warning', 'Security', 'eval() executes arbitrary strings as code.',
          'Parse the data explicitly (for example JSON.parse) or use a lookup table instead of eval().');
      }
      if (/\bdocument\.write\s*\(/.test(s)) {
        r('warning', 'Security', 'document.write() can overwrite the page and is unsafe with untrusted data.',
          'Use DOM APIs such as createElement and appendChild, or textContent for text.');
      }
      if (/\.innerHTML\s*=/.test(s)) {
        r('warning', 'Security', 'Assigning innerHTML can create an XSS hole if the value is user-controlled.',
          'Use textContent for text, or sanitize the HTML before inserting it.');
      }
      if (/\bsetTimeout\s*\(\s*["'`]/.test(s) || /\bsetInterval\s*\(\s*["'`]/.test(s)) {
        r('warning', 'Security', 'setTimeout/setInterval with a string argument behaves like eval().',
          'Pass a function instead, for example setTimeout(() => doWork(), 100).');
      }
      if (/\bdebugger\b\s*;?\s*$/.test(s)) {
        r('warning', 'Maintenance', 'Leftover debugger statement.',
          'Remove the debugger statement before shipping.');
      }
      if (/\bconsole\.log\s*\(/.test(s)) {
        r('info', 'Maintenance', 'console.log left in code.',
          'Remove debug logging or use a proper logger with levels.');
      }
      if (/^\s*var\s+/.test(s)) {
        r('info', 'Style', 'var has function scope and is easy to misuse.',
          'Prefer const (or let when the value changes) for block-scoped variables.');
      }
      if (/\bparseInt\s*\(\s*[^,()]+\)/.test(s)) {
        r('info', 'Logic', 'parseInt() called without a radix.',
          'Pass the radix explicitly, for example parseInt(value, 10).');
      }
      if (/\bwith\s*\(/.test(s)) {
        r('warning', 'Logic', '"with" statement is disallowed in strict mode and hard to read.',
          'Reference the object\'s properties directly, or destructure what you need.');
      }
      if (/\bcatch\s*\([^)]*\)\s*\{\s*\}/.test(s)) {
        r('warning', 'Logic', 'Empty catch block silently swallows errors.',
          'At least log the error, or add a comment explaining why it is safe to ignore.');
      }
      if (/\bnew\s+Array\s*\(\s*\)/.test(s)) {
        r('info', 'Style', 'new Array() is clearer as an array literal.',
          'Use [] instead of new Array().');
      }
      if (/^\s*(const|let|var)\s+[^=;]+=[^;{]*[^;{,(\[\s]\s*$/.test(s) && !/[=+\-*/&|,({\[]\s*$/.test(s)) {
        r('info', 'Style', 'Statement appears to be missing a semicolon.',
          'Add a semicolon at the end of the statement to avoid automatic-semicolon-insertion surprises.');
      }
    });
    return issues;
  }

  // ---------------------------------------------------------------------------
  // Python rules
  // ---------------------------------------------------------------------------
  var BLOCK_HEADER = /^\s*(def|class|if|elif|else|for|while|try|except|finally|with)\b/;
  function stripBrackets(s) {
    return s.replace(/\([^()]*\)|\[[^\[\]]*\]|\{[^{}]*\}/g, '');
  }

  function pythonChecks(ctx) {
    var issues = [];
    var raw = ctx.rawLines;
    var lines = splitLines(ctx.stripped);
    lines.forEach(function (s, idx) {
      var ln = idx + 1;
      var r = function (sev, cat, msg, rec) { issues.push(makeIssue(sev, cat, ln, msg, rec, raw)); };
      var trimmed = s.trim();
      if (!trimmed || trimmed.charAt(0) === '#') return;

      if (/^\s*print\s+["'\w]/.test(s) && !/^\s*print\s*\(/.test(s)) {
        r('error', 'Syntax', 'Python 2 print statement; this is a syntax error in Python 3.',
          'Use print(...) with parentheses.');
      }
      if (/^\s*except\s*:/.test(s)) {
        r('warning', 'Logic', 'Bare "except:" catches everything, including KeyboardInterrupt and SystemExit.',
          'Catch specific exceptions, for example except ValueError:, or at least except Exception:.');
      }
      if (/^\s*except\b[^:]*:\s*$/.test(s) && ctx.nextNonEmpty(idx) && /^\s*pass\s*$/.test(ctx.nextNonEmpty(idx))) {
        r('warning', 'Logic', 'Exception is caught and silently ignored with "pass".',
          'Log the exception or handle it explicitly so failures are visible.');
      }
      if (/\bdef\s+\w+\s*\(.*=\s*(\[\]|\{\}|set\(\))/.test(s)) {
        r('warning', 'Logic', 'Mutable default argument is shared between calls.',
          'Use None as the default and create the list/dict inside the function body.');
      }
      if (/[=!]=\s*None\b/.test(s)) {
        r('info', 'Style', 'Comparing to None with == or !=.',
          'Use "is None" or "is not None".');
      }
      if (/[=!]=\s*(True|False)\b/.test(s)) {
        r('info', 'Style', 'Comparing to a boolean literal.',
          'Use the value directly (if x:) or "if not x:".');
      }
      if (/\b(eval|exec)\s*\(/.test(s)) {
        r('warning', 'Security', 'eval()/exec() run arbitrary code.',
          'Use ast.literal_eval for literals, or an explicit parser for untrusted input.');
      }
      if (/\bpickle\.loads?\s*\(/.test(s)) {
        r('warning', 'Security', 'pickle can execute code when loading untrusted data.',
          'Use json or another safe format for data that crosses trust boundaries.');
      }
      if (/\bsubprocess\.\w+\(.*shell\s*=\s*True/.test(s)) {
        r('warning', 'Security', 'subprocess with shell=True is vulnerable to shell injection.',
          'Pass the command as a list of arguments and keep shell=False.');
      }
      if (/^\s*from\s+\S+\s+import\s+\*/.test(s)) {
        r('info', 'Style', 'Wildcard import hides where names come from.',
          'Import the specific names you need.');
      }
      if (/^\s*assert\s*\(.*,.*\)\s*$/.test(s)) {
        r('error', 'Logic', 'assert with a parenthesized tuple is always true.',
          'Write assert condition, "message" without wrapping them in parentheses.');
      }
      if (/\brange\s*\(\s*len\s*\(/.test(s)) {
        r('info', 'Style', 'range(len(x)) is often clearer as direct iteration.',
          'Use "for item in x" or "for i, item in enumerate(x)".');
      }
      // Block header without a trailing colon (one-liners like "if x: y" are fine)
      if (BLOCK_HEADER.test(s) && !/:\s*(#.*)?$/.test(s) && !/:\s*\S/.test(stripBrackets(s)) &&
          !/[\\(\[{,]\s*$/.test(s)) {
        r('error', 'Syntax', 'Block statement is missing a trailing colon ":".',
          'Add ":" at the end of the line that starts this block.');
      }
      // Indentation that is not a multiple of 4 spaces
      var indent = (s.match(/^( *)\S/) || [, ''])[1].length;
      if (indent > 0 && indent % 4 !== 0) {
        r('info', 'Style', 'Indentation of ' + indent + ' spaces is not a multiple of 4.',
          'Use 4 spaces per indentation level (PEP 8).');
      }
    });
    return issues;
  }

  // ---------------------------------------------------------------------------
  // Java / C shared rules (semicolon + statement heuristics)
  // ---------------------------------------------------------------------------
  function missingSemicolonCheck(ctx, lang) {
    var issues = [];
    var raw = ctx.rawLines;
    var lines = splitLines(ctx.stripped);
    lines.forEach(function (s, idx) {
      var t = s.trim();
      if (!t || t[0] === '#' || t[0] === '@' || t[0] === '*' || /^\/\//.test(t)) return;
      if (/[;{}:,(\[+\-*/%&|=<>?!\\]$/.test(t)) return;          // already terminated or continued
      if (/^(if|for|while|switch|else|do|try|catch|finally|case|default|class|interface|enum|struct|namespace|public|private|protected|static|#)\b/.test(t) && !/\)\s*$/.test(t)) return;
      if (/^(if|for|while|switch|catch)\b.*\)\s*$/.test(t)) return;  // control line without body
      if (/^(\}|\)|\]|else\b)/.test(t)) return;
      if (/\)\s*(const\s*)?$/.test(t) && /^[\w<>\[\], .*&:]+\s+\w+\s*\(/.test(t)) return; // method header
      if (/^(public|private|protected|static|final|abstract|void|int|long|double|float|char|bool|boolean|String|var|const|let|return|[\w.<>\[\]]+\s+\w+\s*=|\w+\s*=|\w+\(|\w+\.\w+\()/.test(t) ||
          /^return\b/.test(t)) {
        if (/^(public|private|protected|static|void|class)\b.*\)\s*$/.test(t)) return; // method header
        issues.push(makeIssue('error', 'Syntax', idx + 1,
          'Statement appears to be missing a terminating semicolon ";".',
          'End the statement with ";". ' + (lang === 'c' ? 'C requires semicolons after each statement.' : 'Java requires semicolons after each statement.'),
          raw));
      }
    });
    return issues;
  }

  function javaChecks(ctx) {
    var issues = missingSemicolonCheck(ctx, 'java');
    var raw = ctx.rawLines;
    splitLines(ctx.stripped).forEach(function (s, idx) {
      var ln = idx + 1;
      var r = function (sev, cat, msg, rec) { issues.push(makeIssue(sev, cat, ln, msg, rec, raw)); };
      if (/\bSystem\.out\.print/.test(s)) {
        r('info', 'Maintenance', 'System.out.print used for output.',
          'Use a logging framework (java.util.logging, SLF4J) for anything beyond quick debugging.');
      }
      if (/\bSystem\.exit\s*\(/.test(s)) {
        r('warning', 'Logic', 'System.exit() terminates the whole JVM.',
          'Return an error code or throw an exception so callers can decide what to do.');
      }
      if (/\bcatch\s*\([^)]*\)\s*\{\s*\}/.test(s)) {
        r('warning', 'Logic', 'Empty catch block silently swallows exceptions.',
          'Log the exception or rethrow it with context.');
      }
      if (/[^=!<>]==\s*"|"\s*==[^=]/.test(s)) {
        r('warning', 'Logic', 'Strings compared with == compare object references, not contents.',
          'Use "value".equals(other) or Objects.equals(a, b).');
      }
      if (/\bif\s*\(\s*[\w.\[\]]+\s*=[^=]/.test(s)) {
        r('error', 'Logic', 'Assignment (=) used inside an if condition.',
          'Use == for comparison, or move the assignment out of the condition.');
      }
      if (/\bThread\.sleep\s*\(/.test(s)) {
        r('info', 'Logic', 'Thread.sleep() blocks the current thread.',
          'Avoid sleeping on the UI or request thread; use a scheduler or async callback.');
      }
      if (/\bcatch\s*\(\s*Exception\s+\w+\s*\)/.test(s)) {
        r('info', 'Logic', 'Catching the broad Exception type.',
          'Catch the specific exception types you expect.');
      }
    });
    return issues;
  }

  function cChecks(ctx) {
    var issues = missingSemicolonCheck(ctx, 'c');
    var raw = ctx.rawLines;
    splitLines(ctx.stripped).forEach(function (s, idx) {
      var ln = idx + 1;
      var r = function (sev, cat, msg, rec) { issues.push(makeIssue(sev, cat, ln, msg, rec, raw)); };
      if (/\bgets\s*\(/.test(s)) {
        r('error', 'Security', 'gets() has no bounds checking and is removed from modern C standards.',
          'Use fgets(buffer, sizeof buffer, stdin) instead.');
      }
      if (/\b(strcpy|strcat|sprintf)\s*\(/.test(s)) {
        r('warning', 'Security', 'Unbounded string function (strcpy/strcat/sprintf) can overflow a buffer.',
          'Use strncpy/strncat/snprintf with the buffer size, or a safer library.');
      }
      if (/\bscanf\s*\(\s*"%s"/.test(s)) {
        r('warning', 'Security', 'scanf("%s") reads without a width limit.',
          'Use a width specifier such as %99s that matches the buffer size.');
      }
      if (/\bprintf\s*\(\s*[a-zA-Z_]\w*\s*\)/.test(s)) {
        r('warning', 'Security', 'Passing a variable directly as the printf format string.',
          'Use printf("%s", variable) so user data cannot act as a format string.');
      }
      if (/\bif\s*\(\s*[\w.\[\]>-]+\s*=[^=]/.test(s)) {
        r('error', 'Logic', 'Assignment (=) used inside an if condition.',
          'Use == for comparison, or move the assignment out of the condition.');
      }
      var alloc = /\b(\w+)\s*=\s*(?:\([^)]*\)\s*)?(malloc|calloc|realloc)\s*\(/.exec(s);
      if (alloc) {
        var v = alloc[1].replace(/[$]/g, '\\$&');
        var checked = new RegExp('(!\\s*' + v + '\\b)|(\\b' + v + '\\s*[!=]=\\s*NULL)|(NULL\\s*[!=]=\\s*' + v + '\\b)|(if\\s*\\(\\s*' + v + '\\s*\\))').test(ctx.stripped);
        if (!checked) {
          r('info', 'Logic', 'Result of ' + alloc[2] + '() is stored in "' + alloc[1] + '" but never checked for NULL.',
            'Check the pointer returned by malloc/calloc/realloc before using it.');
        }
      }
    });
    return issues;
  }

  // ---------------------------------------------------------------------------
  // JSON
  // ---------------------------------------------------------------------------
  function jsonChecks(ctx) {
    var issues = [];
    try {
      JSON.parse(ctx.code);
    } catch (e) {
      var m = /position (\d+)/.exec(e.message || '');
      var line = 1;
      if (m) line = lineOf(ctx.code, parseInt(m[1], 10));
      var reason = (e.message || 'syntax error').split(' in JSON')[0].split(', "')[0];
      issues.push(makeIssue('error', 'Syntax', line,
        'Invalid JSON (' + reason + ').',
        'Check for trailing commas, single quotes (JSON requires double quotes), and unclosed brackets.',
        ctx.rawLines));
    }
    return issues;
  }

  // ---------------------------------------------------------------------------
  // HTML (light checks)
  // ---------------------------------------------------------------------------
  function htmlChecks(ctx) {
    var issues = [];
    var raw = ctx.rawLines;
    var voidTags = { br: 1, img: 1, hr: 1, input: 1, meta: 1, link: 1, area: 1, base: 1, col: 1, embed: 1, source: 1, track: 1, wbr: 1 };
    var stack = [];
    var re = /<(\/?)([a-zA-Z][\w-]*)([^>]*)>/g;
    var m;
    while ((m = re.exec(ctx.stripped))) {
      var tag = m[2].toLowerCase();
      if (voidTags[tag] || /\/\s*$/.test(m[3])) continue;
      var ln = lineOf(ctx.stripped, m.index);
      if (m[1] === '') {
        stack.push({ tag: tag, line: ln });
      } else {
        var idx = -1;
        for (var k = stack.length - 1; k >= 0; k--) if (stack[k].tag === tag) { idx = k; break; }
        if (idx === -1) {
          issues.push(makeIssue('error', 'Syntax', ln,
            'Closing tag </' + tag + '> has no matching opening tag.',
            'Remove the stray closing tag or add the missing opening tag.', raw));
        } else {
          stack.length = idx;
        }
      }
    }
    stack.forEach(function (o) {
      issues.push(makeIssue('warning', 'Syntax', o.line,
        'Tag <' + o.tag + '> is never closed.',
        'Add the closing </' + o.tag + '> tag.', raw));
    });
    if (/<img\b(?![^>]*\balt=)[^>]*>/i.test(ctx.stripped)) {
      var imgLine = lineOf(ctx.stripped, ctx.stripped.search(/<img\b(?![^>]*\balt=)/i));
      issues.push(makeIssue('warning', 'Accessibility', imgLine,
        '<img> without an alt attribute.',
        'Add alt="description" (or alt="" for purely decorative images).', raw));
    }
    if (/\son\w+\s*=\s*["']/i.test(ctx.stripped)) {
      issues.push(makeIssue('info', 'Security', lineOf(ctx.stripped, ctx.stripped.search(/\son\w+\s*=/i)),
        'Inline event handler attribute found (onclick, onload, ...).',
        'Attach event listeners from a script file; this also makes a Content-Security-Policy easier.', raw));
    }
    return issues;
  }

  // ---------------------------------------------------------------------------
  // Main entry
  // ---------------------------------------------------------------------------
  function analyze(code, options) {
    var opts = options || {};
    var source = typeof code === 'string' ? code : '';
    var language = opts.language && opts.language !== 'auto' ? opts.language : detectLanguage(source);
    if (LANGUAGES.indexOf(language) === -1) language = 'javascript';

    var rawLines = splitLines(source);
    var stripped = { text: source, unclosed: [] };
    if (language !== 'json' && language !== 'html') {
      stripped = stripCodeForRules(source, language);
    } else {
      stripped = { text: source, unclosed: [] };
    }

    var ctx = {
      code: source,
      rawLines: rawLines,
      stripped: stripped.text,
      unclosed: stripped.unclosed,
      nextNonEmpty: function (idx) {
        for (var k = idx + 1; k < rawLines.length; k++) {
          var s = splitLines(stripped.text)[k];
          if (s !== undefined && s.trim()) return s;
        }
        return null;
      }
    };
    // Cache the stripped line array for nextNonEmpty.
    var strippedLinesCache = splitLines(stripped.text);
    ctx.nextNonEmpty = function (idx) {
      for (var k = idx + 1; k < strippedLinesCache.length; k++) {
        if (strippedLinesCache[k].trim()) return strippedLinesCache[k];
      }
      return null;
    };

    var issues = [];
    if (language === 'json') {
      issues = jsonChecks(ctx);
    } else if (language === 'html') {
      issues = htmlChecks(ctx);
      issues = issues.concat(genericChecks(ctx).filter(function (i) { return i.category !== 'Syntax'; }));
    } else {
      issues = genericChecks(ctx);
      if (language === 'javascript') issues = issues.concat(javascriptChecks(ctx));
      if (language === 'python') issues = issues.concat(pythonChecks(ctx));
      if (language === 'java') issues = issues.concat(javaChecks(ctx));
      if (language === 'c') issues = issues.concat(cChecks(ctx));
    }

    // Stable order: by line, then by severity (errors first)
    issues.sort(function (a, b) {
      if (a.line !== b.line) return a.line - b.line;
      return SEVERITY[b.severity] - SEVERITY[a.severity];
    });

    var counts = { error: 0, warning: 0, info: 0 };
    issues.forEach(function (i) { counts[i.severity]++; });
    var score = Math.max(0, 100 - counts.error * 12 - counts.warning * 5 - counts.info * 1);
    var verdict;
    if (!source.trim()) verdict = 'Nothing to review yet.';
    else if (counts.error > 0) verdict = 'Needs fixes: ' + counts.error + ' error(s) will likely prevent the code from running.';
    else if (counts.warning > 0) verdict = 'Runs, but review ' + counts.warning + ' warning(s) for bugs or risks.';
    else if (counts.info > 0) verdict = 'Looks good. Only style and maintenance suggestions.';
    else verdict = 'No issues found by the rule set.';

    return {
      language: language,
      lineCount: rawLines.length,
      issues: issues,
      counts: counts,
      score: score,
      verdict: verdict,
      generatedAt: new Date().toISOString()
    };
  }

  function formatReport(result) {
    var out = [];
    out.push('Code Review (' + result.language + ', ' + result.lineCount + ' lines)');
    out.push('Score: ' + result.score + '/100');
    out.push(result.verdict);
    out.push('');
    if (!result.issues.length) out.push('No issues found.');
    result.issues.forEach(function (i) {
      out.push('[' + i.severity.toUpperCase() + '] Line ' + i.line + ' - ' + i.message);
      out.push('    Recommendation: ' + i.recommendation);
    });
    return out.join('\n');
  }

  return {
    analyze: analyze,
    detectLanguage: detectLanguage,
    formatReport: formatReport,
    LANGUAGES: LANGUAGES
  };
});
