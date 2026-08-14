'use strict';

const { spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');

function packageRootFromBinDir(binDir) {
  return path.join(binDir, '..');
}

function resolveNative(fileName, rootDir) {
  if (typeof fileName !== 'string' || fileName.length === 0) {
    throw new Error('native file name is required');
  }
  if (fileName !== path.basename(fileName) || /[\\/]/.test(fileName)) {
    throw new Error('native file name must be a basename');
  }
  return path.join(rootDir, 'native', fileName);
}

function missingMessage(exePath) {
  return `spacelens: native executable not found: ${exePath}\n`;
}

function runNative(fileName, argv, options) {
  const opts = options || {};
  const rootDir = opts.packageRoot || packageRootFromBinDir(__dirname);
  const exePath = opts.nativePath || resolveNative(fileName, rootDir);
  const stdio = opts.stdio || 'inherit';
  const args = Array.isArray(argv) ? argv : [];

  if (!fs.existsSync(exePath)) {
    const stderr = missingMessage(exePath);
    if (stdio !== 'pipe') {
      process.stderr.write(stderr);
    }
    return {
      status: 1,
      signal: null,
      stdout: '',
      stderr,
      error: new Error('native executable not found'),
      exePath,
    };
  }

  const result = spawnSync(exePath, args, {
    stdio,
    shell: false,
    windowsHide: false,
    encoding: stdio === 'pipe' ? opts.encoding || 'utf8' : undefined,
  });

  if (result.error) {
    const stderr = `spacelens: failed to start native executable: ${result.error.message}\n`;
    if (stdio !== 'pipe') {
      process.stderr.write(stderr);
    }
    return {
      status: 1,
      signal: null,
      stdout: result.stdout || '',
      stderr: `${result.stderr || ''}${stderr}`,
      error: result.error,
      exePath,
    };
  }

  return {
    status: result.status === null ? 1 : result.status,
    signal: result.signal,
    stdout: result.stdout || '',
    stderr: result.stderr || '',
    error: null,
    exePath,
  };
}

function exitWithNative(fileName, argv) {
  return runNative(fileName, argv, { stdio: 'inherit' }).status;
}

module.exports = {
  packageRootFromBinDir,
  resolveNative,
  runNative,
  exitWithNative,
};
