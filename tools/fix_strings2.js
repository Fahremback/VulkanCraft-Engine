const fs = require('fs');
const path = 'tools/external-project-advanced/main.cpp';
let content = fs.readFileSync(path, 'utf8');

// Fix: '"save failed: " << error <<\n"' should be '"save failed: " << error << "\\n"'
content = content.replace(/<< error <<\n"/g, '<< error << "\\n"');

fs.writeFileSync(path, content, 'utf8');
console.log('Fixed');
