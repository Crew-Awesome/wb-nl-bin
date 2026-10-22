const Mocha = require('mocha');
const fs = require('fs');
const path = require('path');

const mocha = new Mocha();
const testDir = '.';
const specModule = process.argv[2] || '';
const specFiles = fs.readdirSync(testDir)
    .filter((file) => file.endsWith('.spec.js'))
    .filter((file) => !specModule || file === `${specModule}.spec.js`);

if(specFiles.length === 0) {
    console.error(`No spec found for "${specModule}"`);
    process.exitCode = 1;
}
else {
    specFiles.forEach((file) => mocha.addFile(path.join(testDir, file)));
    mocha.timeout(20000);
    mocha.run((failures) => {
        process.exitCode = failures ? 1 : 0;
    });
}
