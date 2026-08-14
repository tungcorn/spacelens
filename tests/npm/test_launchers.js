'use strict';

const assert = require('assert');
const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const repoRoot = path.join(__dirname, '..', '..');
const launchPath = path.join(repoRoot, 'packaging', 'npm', 'bin', 'launch.js');
const cliLauncher = path.join(repoRoot, 'packaging', 'npm', 'bin', 'spacelens.js');
const guiLauncher = path.join(repoRoot, 'packaging', 'npm', 'bin', 'spacelens-gui.js');
const mcpLauncher = path.join(repoRoot, 'packaging', 'npm', 'bin', 'spacelens-mcp.js');
const { resolveNative, runNative } = require(launchPath);

const scratch = [];
function makePackageTree() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'spacelens-npm-launch-'));
  fs.mkdirSync(path.join(root, 'native'), { recursive: true });
  fs.mkdirSync(path.join(root, 'bin'), { recursive: true });
  scratch.push(root);
  return root;
}

function writeMockNative(root, fileName) {
  const dest = path.join(root, 'native', fileName);
  fs.copyFileSync(process.execPath, dest);
  return dest;
}

function cleanup() {
  for (const dir of scratch) {
    try {
      fs.rmSync(dir, { recursive: true, force: true });
    } catch (_) {
      /* best-effort */
    }
  }
}

let failed = 0;
function test(name, fn) {
  try {
    fn();
    process.stdout.write(`ok  ${name}\n`);
  } catch (err) {
    failed += 1;
    process.stderr.write(`not ok  ${name}\n${err && err.stack ? err.stack : err}\n`);
  }
}

test('resolveNative uses package-local native basename', () => {
  const root = makePackageTree();
  const resolved = resolveNative('spacelens.exe', root);
  assert.strictEqual(resolved, path.join(root, 'native', 'spacelens.exe'));
  const gui = resolveNative('spacelens-gui.exe', root);
  assert.strictEqual(gui, path.join(root, 'native', 'spacelens-gui.exe'));
  const mcp = resolveNative('spacelens-mcp.exe', root);
  assert.strictEqual(mcp, path.join(root, 'native', 'spacelens-mcp.exe'));
});

test('resolveNative rejects path separators', () => {
  const root = makePackageTree();
  assert.throws(() => resolveNative('..\\evil.exe', root));
  assert.throws(() => resolveNative('sub/spacelens.exe', root));
  assert.throws(() => resolveNative('', root));
});

test('missing native executable writes stderr and exits 1', () => {
  const root = makePackageTree();
  const result = runNative('spacelens.exe', ['version'], {
    packageRoot: root,
    stdio: 'pipe',
  });
  assert.strictEqual(result.status, 1);
  assert.match(result.stderr, /native executable not found/);
  assert.strictEqual(result.stdout, '');
});

const mockRoot = makePackageTree();
writeMockNative(mockRoot, 'spacelens.exe');
writeMockNative(mockRoot, 'spacelens-gui.exe');

function runEcho(args) {
  const script =
    'process.stdout.write(JSON.stringify(process.argv.slice(1))); process.exit(0);';
  return runNative('spacelens.exe', ['-e', script].concat(args), {
    packageRoot: mockRoot,
    stdio: 'pipe',
  });
}

test('argument with spaces is not split', () => {
  const result = runEcho(['path with spaces', 'ok']);
  assert.strictEqual(result.status, 0);
  assert.deepStrictEqual(JSON.parse(result.stdout), ['path with spaces', 'ok']);
});

test('Unicode argument is preserved', () => {
  const uni = 'café-文件-Δ';
  const result = runEcho([uni]);
  assert.strictEqual(result.status, 0);
  assert.deepStrictEqual(JSON.parse(result.stdout), [uni]);
});

test('quotes are not shell-reparsed', () => {
  const quoted = 'say "hello"';
  const result = runEcho([quoted]);
  assert.strictEqual(result.status, 0);
  assert.deepStrictEqual(JSON.parse(result.stdout), [quoted]);
});

test('stdout is preserved without a wrapper banner', () => {
  const result = runNative('spacelens.exe', ['-e', "process.stdout.write('native-out'); process.exit(0);"], {
    packageRoot: mockRoot,
    stdio: 'pipe',
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stdout, 'native-out');
  assert.doesNotMatch(result.stdout, /spacelens:|npm|wrapper/i);
});

test('stderr is preserved', () => {
  const result = runNative('spacelens.exe', ['-e', "process.stderr.write('native-err'); process.exit(0);"], {
    packageRoot: mockRoot,
    stdio: 'pipe',
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.stderr, 'native-err');
});

test('native exit code is preserved', () => {
  const result = runNative('spacelens.exe', ['-e', 'process.exit(7);'], {
    packageRoot: mockRoot,
    stdio: 'pipe',
  });
  assert.strictEqual(result.status, 7);
});

test('GUI launcher resolves native/spacelens-gui.exe', () => {
  const resolved = resolveNative('spacelens-gui.exe', mockRoot);
  assert.ok(resolved.endsWith(path.join('native', 'spacelens-gui.exe')));
  const result = runNative('spacelens-gui.exe', ['-e', 'process.exit(0);'], {
    packageRoot: mockRoot,
    stdio: 'pipe',
  });
  assert.strictEqual(result.status, 0);
  assert.strictEqual(result.exePath, resolved);
});

test('CLI launcher entry does not print a banner around missing native', () => {
  const empty = makePackageTree();
  const result = spawnSync(process.execPath, [cliLauncher, 'version'], {
    cwd: empty,
    shell: false,
    encoding: 'utf8',
  });
  assert.notStrictEqual(result.status, 0);
  assert.match(result.stderr, /native executable not found/);
  assert.strictEqual(result.stdout, '');
});

test('GUI launcher entry targets spacelens-gui.exe', () => {
  const text = fs.readFileSync(guiLauncher, 'utf8');
  assert.match(text, /spacelens-gui\.exe/);
  assert.doesNotMatch(text, /shell:\s*true/);
});

test('MCP launcher entry targets spacelens-mcp.exe and prints no banner', () => {
  const text = fs.readFileSync(mcpLauncher, 'utf8');
  assert.match(text, /spacelens-mcp\.exe/);
  assert.doesNotMatch(text, /shell:\s*true/);
  assert.doesNotMatch(text, /Starting SpaceLens|console\.log/);
  const empty = makePackageTree();
  const result = spawnSync(process.execPath, [mcpLauncher], {
    cwd: empty,
    shell: false,
    encoding: 'utf8',
  });
  assert.notStrictEqual(result.status, 0);
  assert.match(result.stderr, /native executable not found/);
  assert.strictEqual(result.stdout, '');
});

test('launchers do not concatenate argv into a shell string', () => {
  const text = [
    fs.readFileSync(launchPath, 'utf8'),
    fs.readFileSync(cliLauncher, 'utf8'),
    fs.readFileSync(guiLauncher, 'utf8'),
    fs.readFileSync(mcpLauncher, 'utf8'),
  ].join('\n');
  assert.match(text, /shell:\s*false/);
  assert.doesNotMatch(text, /child_process\.exec(?:Sync)?\s*\(/);
  assert.doesNotMatch(text, /argv\.join\(/);
});

cleanup();

if (failed) {
  process.stderr.write(`${failed} launcher test(s) failed\n`);
  process.exit(1);
}
process.stdout.write('launcher tests PASS\n');
process.exit(0);
