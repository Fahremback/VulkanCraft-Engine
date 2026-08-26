const fs = require('fs');
const path = 'tools/external-project-advanced/main.cpp';
let content = fs.readFileSync(path, 'utf8');

// Replace literal newlines inside cerr strings
// Pattern: "text\n" (literal LF) -> "text\\n"
content = content.replace(/timed out\n"/g, 'timed out\\n"');
content = content.replace(/save failed: " << error <<\n"/g, 'save failed: " << error << "\\n"');
content = content.replace(/load failed: " << error <<\n"/g, 'load failed: " << error << "\\n"');

fs.writeFileSync(path, content, 'utf8');
console.log('Fixed');
