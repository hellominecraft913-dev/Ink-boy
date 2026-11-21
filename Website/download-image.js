import fetch from 'node-fetch';
import fs from 'fs';

const url = 'https://github.com/hellominecraft913-dev/ink-boy/raw/main/assets/pokemon-red.png';
const response = await fetch(url);
const buffer = Buffer.from(await response.arrayBuffer());
fs.writeFileSync('C:\\Users\\Abdullah\\Downloads\\minecraft\\assets\\pokemon-red.png', buffer);
console.log('Downloaded');
