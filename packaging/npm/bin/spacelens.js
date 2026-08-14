#!/usr/bin/env node
'use strict';

const { exitWithNative } = require('./launch.js');

process.exit(exitWithNative('spacelens.exe', process.argv.slice(2)));
