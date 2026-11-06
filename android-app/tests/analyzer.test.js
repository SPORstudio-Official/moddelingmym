// Run with: node --test android-app/tests/
const test = require('node:test');
const assert = require('node:assert');
const path = require('path');
const CodeAnalyzer = require(path.join(__dirname, '..', 'app', 'src', 'main', 'assets', 'analyzer.js'));

function has(result, severity, pattern) {
  return result.issues.some(i => i.severity === severity && pattern.test(i.message));
}

test('detects JavaScript language and common bugs', () => {
  const r = CodeAnalyzer.analyze('function f(a, b) {\n  if (a == b) { eval(a) }\n  return a\n');
  assert.strictEqual(r.language, 'javascript');
  assert.ok(has(r, 'error', /never closed/));
  assert.ok(has(r, 'warning', /Loose equality/));
  assert.ok(has(r, 'warning', /eval/));
  assert.ok(r.score < 100);
});

test('flags hard-coded credentials as errors', () => {
  const r = CodeAnalyzer.analyze('const apiKey = "sk_live_123456789";');
  assert.ok(has(r, 'error', /hard-coded credential/));
});

test('ignores brackets and keywords inside strings and comments', () => {
  const code = [
    "const s = '({[ not a bracket';",
    '// if (x == 1) eval(y)',
    '/* { unclosed in comment is closed below */',
    'const t = "}";',
    '/* end */',
  ].join('\n');
  const r = CodeAnalyzer.analyze(code, { language: 'javascript' });
  assert.ok(!r.issues.some(i => i.severity === 'error'), JSON.stringify(r.issues));
});

test('reports mismatched and unexpected brackets with line numbers', () => {
  const r = CodeAnalyzer.analyze('let a = [1, 2);\nlet b = 3);');
  const mismatch = r.issues.find(i => /Mismatched/.test(i.message));
  assert.ok(mismatch);
  assert.strictEqual(mismatch.line, 1);
  assert.ok(r.issues.some(i => /Unexpected closing/.test(i.message) && i.line === 2));
});

test('reports an unclosed string literal', () => {
  const r = CodeAnalyzer.analyze('let s = "hello;\nlet t = 1;');
  assert.ok(has(r, 'error', /String literal is not closed/));
});

test('detects Python 2 print, bare except and missing colon', () => {
  const code = 'def f(x):\n    print "hi"\n    try:\n        pass\n    except:\n        pass\n    if x == None\n        return x\n';
  const r = CodeAnalyzer.analyze(code, { language: 'python' });
  assert.ok(has(r, 'error', /Python 2 print/));
  assert.ok(has(r, 'warning', /Bare "except:"/));
  assert.ok(has(r, 'error', /missing a trailing colon/));
  assert.ok(has(r, 'info', /None/));
});

test('detects mutable default arguments in Python', () => {
  const r = CodeAnalyzer.analyze('def add(item, items=[]):\n    items.append(item)\n    return items\n', { language: 'python' });
  assert.ok(has(r, 'warning', /Mutable default/));
});

test('detects missing semicolons in Java and string == comparison', () => {
  const code = 'public class A {\n  void m(String s) {\n    int n = 0\n    if (s == "x") { }\n  }\n}\n';
  const r = CodeAnalyzer.analyze(code, { language: 'java' });
  assert.ok(has(r, 'error', /missing a terminating semicolon/));
  assert.ok(has(r, 'warning', /compare/));
});

test('detects unsafe C functions', () => {
  const r = CodeAnalyzer.analyze('#include <stdio.h>\nint main(void) {\n  char b[8];\n  gets(b);\n  return 0;\n}\n', { language: 'c' });
  assert.ok(has(r, 'error', /gets\(\)/));
});

test('C allocation with NULL check does not raise a warning', () => {
  const r = CodeAnalyzer.analyze('int main(void) {\n  int *p = malloc(4);\n  if (p == NULL) return 1;\n  free(p);\n  return 0;\n}\n', { language: 'c' });
  assert.ok(!r.issues.some(i => /never checked for NULL/.test(i.message)));
});

test('validates JSON and reports syntax errors', () => {
  const bad = CodeAnalyzer.analyze('{"a": [1,2,],}', { language: 'json' });
  assert.ok(has(bad, 'error', /Invalid JSON/));
  const good = CodeAnalyzer.analyze('{"a": [1,2]}', { language: 'json' });
  assert.strictEqual(good.issues.length, 0);
});

test('auto-detects JSON, Python and Java', () => {
  assert.strictEqual(CodeAnalyzer.detectLanguage('{"a": 1}'), 'json');
  assert.strictEqual(CodeAnalyzer.detectLanguage('def main():\n    pass\n'), 'python');
  assert.strictEqual(CodeAnalyzer.detectLanguage('public class X {\n  System.out.println("x");\n}'), 'java');
});

test('empty input yields a neutral result', () => {
  const r = CodeAnalyzer.analyze('   ');
  assert.strictEqual(r.counts.error, 0);
  assert.match(r.verdict, /Nothing to review/);
});

test('report contains recommendations for every issue', () => {
  const r = CodeAnalyzer.analyze('var x = 1; console.log(x)  \n');
  const text = CodeAnalyzer.formatReport(r);
  assert.match(text, /Recommendation:/);
  assert.strictEqual((text.match(/Recommendation:/g) || []).length, r.issues.length);
});
