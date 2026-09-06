import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { inflateSync } from 'node:zlib';
const req = createRequire(import.meta.url);
const COMPACT = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/libma5t_compact';
const MMF = 'D:/working/vscode-projects/YM2163-Midi/Denjhang_Music_Player_v16/ma2play/bin/mmf/Samsung/Samsung SGH-D500 ringtones/Pre-downloaded/Melody05.mmf';
const m = await req('./build/ma5play_node.js')({ printErr: () => {} });   // 吞掉 init 噪音? 不行——要 bass trace
