#!/usr/bin/env node
'use strict';

const { exitWithNative } = require('./launch.js');

process.exit(exitWithNative('spacelens-gui.exe', process.argv.slice(2)));
