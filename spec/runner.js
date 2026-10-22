const { execFileSync, spawn, spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const APP_TIMEOUT_MS = 15000;
const APP_EXIT_DELAY_MS = 100;
const APP_MAX_RUNTIME_MS = 10000;
const BIN_DIR = path.resolve(__dirname, '../bin');
const BINARY_NAMES = {
    linux: `neutralino-linux_${process.arch}`,
    darwin: `neutralino-mac_${process.arch}`,
    win32: 'neutralino-win_x64.exe'
};

const SOURCE_TEMPLATE = `
{BEFORE_INIT_CODE}

let __closed = false;
let __maxTimeout;

Neutralino.init();

Neutralino.events.on("ready", async () => {
    await __init();
    {CODE}
});

async function __close(data = "", exitCode = 0) {
    if(__closed) return;
    __closed = true;
    clearTimeout(__maxTimeout);
    try {
        if(data) {
            await Neutralino.filesystem.writeFile(NL_PATH + "/.tmp/output.txt", data);
        }
    }
    finally {
        setTimeout(() => Neutralino.app.exit(exitCode), ${APP_EXIT_DELAY_MS});
    }
}

async function __init() {
    __maxTimeout = setTimeout(async () => {
        try {
            await Neutralino.filesystem.writeFile(
                NL_PATH + "/.tmp/output.txt", 'NL_SP_MAXTIMT');
        }
        finally {
            await Neutralino.app.exit(1); // max timeout force exit
        }
    }, ${APP_MAX_RUNTIME_MS});

    try {
        await Neutralino.filesystem.createDirectory(NL_PATH + "/.tmp");
    }
    catch(err) {
        // ignore
    }
}
`;

let activeRun = null;

function run(code, options = {}) {
    cleanup();
    activeRun = createRunDirectory();
    const sourceFile = path.join(activeRun, 'resources/js/main_spec.js');
    fs.writeFileSync(sourceFile, makeAppSource(code, options.beforeInitCode));

    if(options.debug) {
        console.log('INFO: Running the app from ' + activeRun);
    }

    let exitCode = 0;
    const pidFile = path.join(activeRun, 'app.pid');
    try {
        const command = makeCommand(options.args, activeRun);
        if(options.debug) {
            console.log('INFO: Running command: ' + command.binary + ' ' + command.args.join(' '));
        }
        execFileSync(process.execPath, [
            __filename,
            '--run-child',
            JSON.stringify({
                binary: command.binary,
                args: command.args,
                pidFile
            })
        ], {
            detached: process.platform !== 'win32',
            stdio: 'ignore',
            timeout: APP_TIMEOUT_MS,
            windowsHide: true
        });
    }
    catch(err) {
        if(err.code === 'ETIMEDOUT') {
            const appPid = readPid(pidFile);
            killProcessTree(appPid);
            killProcessTree(err.pid);
            throw new Error(
                `Neutralino test app timed out after ${APP_TIMEOUT_MS}ms`,
                { cause: err }
            );
        }
        if(err.status === null || err.status === undefined) {
            throw err;
        }
        exitCode = err.status;
    }

    if(options.debug) {
        console.log('INFO: Test app was closed with exit code ' + exitCode);
    }
    return exitCode;
}

function getOutput() {
    if(!activeRun) return '';

    const outputFile = path.join(activeRun, '.tmp/output.txt');
    let content = '';
    try {
        content = fs.readFileSync(outputFile, 'utf8');
    }
    finally {
        cleanup();
    }
    return content;
}

function makeCommand(optArgs = '', appPath) {
    const binaryName = BINARY_NAMES[process.platform];
    if(!binaryName) {
        throw new Error(`Unsupported test platform: ${process.platform}`);
    }

    return {
        binary: path.join(BIN_DIR, binaryName),
        args: [
            `--path=${appPath}`,
            '--load-dir-res',
            '--window-exit-process-on-close',
            '--url=/index_spec.html',
            '--window-enable-inspector=false',
            ...splitArgs(optArgs)
        ]
    };
}

function splitArgs(value) {
    if(Array.isArray(value)) return value;
    if(!value) return [];
    return value.match(/(?:[^\s"]|"[^"]*")+/g)
        .map((arg) => arg.replace(/^"|"$/g, ''));
}

function createRunDirectory() {
    const runPath = fs.mkdtempSync(path.join(os.tmpdir(), 'neutralino-spec-'));
    fs.cpSync(path.join(BIN_DIR, 'resources'), path.join(runPath, 'resources'), {
        recursive: true
    });
    fs.cpSync(path.join(BIN_DIR, 'extensions'), path.join(runPath, 'extensions'), {
        recursive: true
    });
    fs.copyFileSync(
        path.join(BIN_DIR, 'neutralino.config.json'),
        path.join(runPath, 'neutralino.config.json')
    );

    for(const file of fs.readdirSync(BIN_DIR)) {
        if(file.endsWith('.config.json') && file !== 'neutralino.config.json') {
            fs.copyFileSync(path.join(BIN_DIR, file), path.join(runPath, file));
        }
    }
    return runPath;
}

function killProcessTree(pid) {
    if(!pid) return;

    if(process.platform === 'win32') {
        spawnSync('taskkill.exe', ['/PID', String(pid), '/T', '/F'], {
            stdio: 'ignore',
            timeout: 5000,
            windowsHide: true
        });
        return;
    }

    try {
        process.kill(-pid, 'SIGKILL');
    }
    catch(err) {
        try {
            process.kill(pid, 'SIGKILL');
        }
        catch(innerErr) {
            // The process may have exited between the timeout and cleanup.
        }
    }
}

function readPid(pidFile) {
    try {
        const pid = Number(fs.readFileSync(pidFile, 'utf8'));
        return Number.isInteger(pid) && pid > 0 ? pid : null;
    }
    catch(err) {
        return null;
    }
}

function runChild(serializedOptions) {
    const options = JSON.parse(serializedOptions);
    const child = spawn(options.binary, options.args, {
        detached: process.platform !== 'win32',
        stdio: 'ignore',
        windowsHide: true
    });

    fs.writeFileSync(options.pidFile, String(child.pid));
    child.once('error', (err) => {
        console.error(err.message);
        process.exitCode = 1;
    });
    child.once('exit', (code) => {
        process.exitCode = typeof code === 'number' ? code : 1;
    });
}

function makeAppSource(code, beforeInitCode = '') {
    return SOURCE_TEMPLATE
        .replace('{CODE}', code)
        .replace('{BEFORE_INIT_CODE}', beforeInitCode);
}

function cleanup() {
    if(!activeRun) return;

    const runPath = activeRun;
    activeRun = null;
    fs.rmSync(runPath, {
        recursive: true,
        force: true,
        maxRetries: 5,
        retryDelay: 100
    });
}

if(process.argv[2] === '--run-child') {
    runChild(process.argv[3]);
}

process.on('exit', cleanup);

module.exports = {
    run,
    getOutput
};
