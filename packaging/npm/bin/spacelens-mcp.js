#!/usr/bin/env node
'use strict';

const { exitWithNative } = require('./launch.js');

process.exit(exitWithNative('spacelens-mcp.exe', process.argv.slice(2)));
