/**
 * C++ MMF/SMAF Parser Implementation
 * Based on smaf825-1 (github.com/but80/smaf825)
 */

#include "mmf_parser.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>

// Log callback (set by application)
void (*mmf_log_fn)(const char* fmt, ...) = NULL;

static void mmf_log(const char* fmt, ...) {
    if (!mmf_log_fn) return;
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    mmf_log_fn("%s", buf);
}

// ============ Helpers ============

static uint32_t read_u32_be(const uint8_t* p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}

static uint16_t read_u16_be(const uint8_t* p) {
    return ((uint16_t)p[0]<<8) | p[1];
}

static int read_variable_int(const uint8_t** pp, int* rest, int allow3bytes) {
    int result = 0;
    int i = 0;
    while (*rest > 0) {
        uint8_t b = **pp;
        (*pp)++;
        (*rest)--;
        if (!allow3bytes && i == 1) {
            return (result + 0x80) | b;
        }
        result |= b & 0x7F;
        if ((b & 0x80) == 0) break;
        result <<= 7;
        i++;
    }
    return result;
}

static int time_base_from_byte(uint8_t b) {
    switch (b) {
        case 0x00: return 1;
        case 0x01: return 2;
        case 0x02: return 4;
        case 0x03: return 5;
        case 0x10: return 10;
        case 0x11: return 20;
        case 0x12: return 40;
        case 0x13: return 50;
        default:   return 2;
    }
}

// ============ Shift-JIS conversion (minimal) ============

// Basic Shift-JIS to UTF-8. Handles ASCII, half-width katakana, and common CJK range.
// sjis_to_utf8 declared extern in header, implemented here
int sjis_to_utf8(const uint8_t* src, int src_len, char* dst, int dst_len) {
    if (!src || !dst || dst_len <= 0) return 0;
    int si = 0, di = 0;
    while (si < src_len && di < dst_len - 3) {
        uint8_t b = src[si];
        if (b < 0x80) {
            // ASCII
            dst[di++] = (char)b;
            si++;
        } else if (b >= 0xA1 && b <= 0xDF) {
            // Half-width katakana -> convert to full-width (3-byte UTF-8)
            // U+FF61..U+FF9F = EF BC 81 .. EF BD 9F
            int base = 0xFF61 + (b - 0xA1);
            dst[di++] = (char)(0xE0 | (base >> 12));
            dst[di++] = (char)(0x80 | ((base >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (base & 0x3F));
            si++;
        } else if (si + 1 < src_len) {
            // Double-byte Shift-JIS -> UTF-8
            uint8_t b2 = src[si + 1];
            // Convert SJIS code point to Unicode (simplified JIS X 0208 table)
            // For proper conversion we'd need a full table, but many SMAF files
            // use ASCII or simple Shift-JIS. We do a basic conversion.
            // Map common SJIS range to Unicode
            unsigned int sjis_code = ((unsigned)b << 8) | b2;
            unsigned int unicode;

            // Basic SJIS to Unicode conversion table (covers common ranges)
            if (sjis_code >= 0x8140 && sjis_code <= 0x84BE) {
                // JIS X 0208 row 16-47
                int row = (b >= 0xA1) ? ((b - 0xA1) * 2 + 0x21) : ((b - 0x81) * 2 + 0x21);
                int col = (b2 >= 0x7F) ? (b2 - 0x41) : (b2 - 0x40);
                if (b2 >= 0x80) col--;
                unicode = 0x4E00 + row * 94 + col; // Rough CJK mapping
            } else if (sjis_code >= 0x889F && sjis_code <= 0x9872) {
                int row = (b - 0x89) * 2 + ((b2 > 0x7F) ? 0x22 : 0x21);
                int col = (b2 >= 0x7F) ? (b2 - 0x41) : (b2 - 0x40);
                if (b2 >= 0x80) col--;
                unicode = 0x4E00 + row * 94 + col;
            } else {
                // Fallback: represent as raw bytes
                dst[di++] = '?';
                si += 2;
                continue;
            }

            // Encode Unicode to UTF-8
            if (unicode < 0x80) {
                dst[di++] = (char)unicode;
            } else if (unicode < 0x800) {
                dst[di++] = (char)(0xC0 | (unicode >> 6));
                dst[di++] = (char)(0x80 | (unicode & 0x3F));
            } else {
                dst[di++] = (char)(0xE0 | (unicode >> 12));
                dst[di++] = (char)(0x80 | ((unicode >> 6) & 0x3F));
                dst[di++] = (char)(0x80 | (unicode & 0x3F));
            }
            si += 2;
        } else {
            dst[di++] = '?';
            si++;
        }
    }
    dst[di] = '\0';
    return di;
}

// ============ Huffman decoder for MobileStandardCompressed ============

class BitReader {
    const uint8_t*& m_src;
    int& m_rest;
    uint8_t m_buf;
    int m_bits;
public:
    BitReader(const uint8_t*& src, int& rest) : m_src(src), m_rest(rest), m_buf(0), m_bits(0) {}

    bool readBit() {
        if (m_bits == 0) {
            if (m_rest <= 0) return false;
            m_buf = *m_src++;
            m_rest--;
            m_bits = 8;
        }
        bool result = (m_buf & 0x80) != 0;
        m_buf <<= 1;
        m_bits--;
        return result;
    }

    uint8_t readUint8() {
        if (m_rest <= 0) return 0;
        uint8_t buf2 = *m_src++;
        m_rest--;
        uint8_t result = m_buf | (buf2 >> m_bits);
        m_buf = buf2 << (8 - m_bits);
        return result;
    }
};

static int huffman_decode(const uint8_t* src, int src_len, uint8_t* dst, int dst_max) {
    const uint8_t* sp = src;
    int srest = src_len;
    BitReader br(sp, srest);

    // Read 4-byte decompressed size (big-endian)
    if (srest < 4) return -1;
    uint32_t out_size = ((uint32_t)sp[0]<<24) | ((uint32_t)sp[1]<<16) | ((uint32_t)sp[2]<<8) | sp[3];
    sp += 4; srest -= 4;

    // Re-create BitReader from remaining data
    BitReader br2(sp, srest);

    // Read Huffman tree
    // left[i] = left child, right[i] = right child, for i >= 256 = internal nodes
    // leaves are values 0-255
    int left[511], right[511];
    int avail = 256;

    // Recursive tree reader using stack
    struct StackEntry { int node; int phase; };
    StackEntry stack[256];
    int sp_idx = 0;

    // readtree: returns node index
    auto readNode = [&](int* node) -> bool {
        if (sp_idx >= 256) return false;
        stack[sp_idx].phase = 0;
        stack[sp_idx].node = -1;
        sp_idx++;

        while (sp_idx > 0) {
            StackEntry& top = stack[sp_idx - 1];
            if (top.phase == 0) {
                bool bit = br2.readBit();
                if (bit) {
                    // Internal node
                    int i = avail++;
                    if (i >= 511) return false;
                    if (top.node == -1) top.node = i;
                    else {
                        // Need to link from parent - simplified: use recursive approach
                    }
                    // Push right child
                    // This stack approach is getting complex, use simpler method
                    return false;
                } else {
                    // Leaf
                    uint8_t val = br2.readUint8();
                    *node = val;
                    sp_idx--;
                    return true;
                }
            }
        }
        return false;
    };

    // Simplified: just allocate a big buffer and do byte-level decode
    // The Huffman tree is embedded in the bitstream. Let's use a different approach.

    // Actually, let's implement this properly with explicit recursion simulation
    // left[256..510] = internal nodes, values 0..255 = leaves

    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));
    avail = 256;

    // Build tree recursively using a manual stack
    // readtree returns a node index (0-255 for leaf, 256+ for internal)
    // We need to simulate recursion.
    // Phase 0: read the bit (0=leaf, 1=internal)
    // Phase 1: left subtree (for internal node)
    // Phase 2: right subtree (for internal node)
    struct { int phase; int my_node; int parent_node; int child_slot; } rstack[256];
    int rsp = 0;

    int root = -1;

    // Start reading tree
    rstack[0].phase = 0;
    rstack[0].my_node = -1;
    rstack[0].parent_node = -1;
    rstack[0].child_slot = 0;
    rsp = 1;

    while (rsp > 0) {
        auto& e = rstack[rsp - 1];

        if (e.phase == 0) {
            bool bit = br2.readBit();
            if (!bit) {
                // Leaf: read 8-bit value
                uint8_t val = br2.readUint8();
                int node = val;

                if (e.parent_node < 0) {
                    root = node; // Single leaf tree
                } else if (e.child_slot == 0) {
                    left[e.parent_node] = node;
                } else {
                    right[e.parent_node] = node;
                }
                e.my_node = node;
                rsp--;
                continue;
            }
            // Internal node
            int inode = avail++;
            if (inode >= 511) { root = -1; break; }
            e.my_node = inode;

            if (e.parent_node < 0) {
                root = inode;
            } else if (e.child_slot == 0) {
                left[e.parent_node] = inode;
            } else {
                right[e.parent_node] = inode;
            }

            // Push left child
            e.phase = 1;
            rstack[rsp].phase = 0;
            rstack[rsp].my_node = -1;
            rstack[rsp].parent_node = inode;
            rstack[rsp].child_slot = 0;
            rsp++;
        } else if (e.phase == 1) {
            // Left child done, push right child
            e.phase = 2;
            rstack[rsp].phase = 0;
            rstack[rsp].my_node = -1;
            rstack[rsp].parent_node = e.my_node;
            rstack[rsp].child_slot = 1;
            rsp++;
        } else {
            rsp--;
        }
    }

    if (root < 0) return -1;

    // Decode data
    int out_count = 0;
    int to_decode = (int)((out_size < (uint32_t)dst_max) ? out_size : dst_max);

    for (int k = 0; k < to_decode; k++) {
        int j = root;
        while (j >= 256) {
            bool b = br2.readBit();
            j = b ? right[j] : left[j];
        }
        dst[k] = (uint8_t)j;
        out_count++;
    }

    return out_count;
}

// ============ Event creation ============

// Short modulation table (from smaf825)
static const int shortModTable[16] = {
    0x00, 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30,
    0x38, 0x40, 0x48, 0x50, 0x60, 0x70, 0x7F, 0x7F,
};

// Short expression table (from smaf825)
static const int shortExpTable[16] = {
    0x00, 0x00, 0x1F, 0x27, 0x2F, 0x37, 0x3F, 0x47,
    0x4F, 0x57, 0x5F, 0x67, 0x6F, 0x77, 0x7F, 0x7F,
};

// Last velocity context per channel
static int g_lastVelocity[16];

static void reset_velocity_context() {
    for (int i = 0; i < 16; i++) g_lastVelocity[i] = 64;
}

// Read exclusive message
static int read_exclusive(const uint8_t** pp, int* rest, int variable_length, MmfExclusive* out) {
    out->type = MMF_EXCL_UNKNOWN;
    out->voice_type = 0;
    out->bank_msb = 0;
    out->bank_lsb = 0;
    out->pc = 0;
    out->drum_note = 0;

    int length;
    if (variable_length) {
        length = read_variable_int(pp, rest, false);
    } else {
        if (*rest <= 0) return -1;
        length = **pp;
        (*pp)++;
        (*rest)--;
    }
    if (length <= 0) return -1;
    length--; // exclude F7

    out->data = *pp;
    out->data_len = length;

    if (*rest < length + 1) return -1;
    *pp += length;
    *rest -= length;

    // Check end mark
    uint8_t end = **pp;
    (*pp)++;
    (*rest)--;
    if (end != 0xF7) {
        // Invalid end mark, include in data
        // (non-fatal, but note it)
    }

    // Detect MA version from exclusive data
    const uint8_t* d = out->data;
    int dlen = out->data_len;

    if (dlen >= 10 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x01) {
        // MA-5 voice: 43 79 07 7F 01 bankMSB bankLSB PC drumNote voiceType
        out->type = MMF_EXCL_VM35_VOICE;
        out->bank_msb = d[5];
        out->bank_lsb = d[6];
        out->pc = d[7];
        out->drum_note = d[8];
        out->voice_type = d[9];
    } else if (dlen >= 10 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x01) {
        // MA-3 voice
        out->type = MMF_EXCL_VM35_VOICE;
        out->bank_msb = d[5];
        out->bank_lsb = d[6];
        out->pc = d[7];
        out->drum_note = d[8];
        out->voice_type = d[9];
    } else if (dlen >= 3 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x01) {
        // MA-5 simplified voice: 43 05 01 bankLSB PC [voiceData]
        out->type = MMF_EXCL_VM35_VOICE;
        out->bank_lsb = d[3];
        out->pc = d[4];
        out->voice_type = 1; // FM
    } else if (dlen >= 3 && d[0] == 0x43 && d[1] == 0x03) {
        // MA-2 voice: 43 03 bank PC [voiceData]
        out->type = MMF_EXCL_VMA_VOICE;
        out->bank_msb = 0;
        out->bank_lsb = d[3];
        out->pc = d[4];
        out->voice_type = 1; // FM
    }

    return 0;
}

// Create event from MobileStandard (16-channel, MIDI-like) format
static int create_event_mobile(const uint8_t** pp, int* rest, MmfEvent* evt) {
    if (*rest <= 0) return -1;
    uint8_t sig = **pp;
    (*pp)++;
    (*rest)--;

    int ch = sig & 0x0F;
    int status = sig & 0xF0;

    switch (status) {
        case 0x80: // Note Off (note on with vel=0)
        case 0x90: // Note On
        {
            if (*rest < 1) return -1;
            uint8_t note = **pp;
            (*pp)++;
            (*rest)--;
            int vel;
            if (status == 0x90) {
                if (*rest < 1) return -1;
                vel = **pp;
                (*pp)++;
                (*rest)--;
                g_lastVelocity[ch] = vel;
            } else {
                vel = g_lastVelocity[ch];
            }
            int dur = read_variable_int(pp, rest, true);
            evt->type = MMF_EVT_NOTE;
            evt->channel = ch;
            evt->note.note = note;
            evt->note.velocity = vel;
            evt->note.gate_time = dur;
            return 0;
        }
        case 0xB0: // Control Change
        {
            if (*rest < 2) return -1;
            uint8_t cc = **pp;
            (*pp)++;
            (*rest)--;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = cc;
            evt->cc.value = val;
            return 0;
        }
        case 0xC0: // Program Change
        {
            if (*rest < 1) return -1;
            uint8_t pc = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_PC;
            evt->channel = ch;
            evt->pc.pc = pc;
            return 0;
        }
        case 0xE0: // Pitch Bend
        {
            if (*rest < 2) return -1;
            uint8_t lo = **pp;
            (*pp)++;
            (*rest)--;
            uint8_t hi = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_PITCHBEND;
            evt->channel = ch;
            evt->pitchbend.value = (int)(lo & 0x7F) | (int)(hi & 0x7F) << 7;
            /* 保留原始 MIDI pitch bend 值 (0-16383, 中心 8192)。
             * smaf_sequencer 按 format_type 统一减 8192 转有符号。
             * (MA-3 同款修复，见 AGENTS.md Section 0.3) */
            return 0;
        }
        case 0xF0: // System
        {
            if (sig == 0xF0) {
                // Exclusive
                evt->type = MMF_EVT_EXCLUSIVE;
                evt->channel = 0;
                MmfExclusive excl;
                if (read_exclusive(pp, rest, true, &excl) != 0) return -1;
                evt->exclusive.data = excl.data;
                evt->exclusive.data_len = excl.data_len;
                return 0;
            }
            if (sig == 0xFF) {
                if (*rest < 1) return -1;
                uint8_t s = **pp;
                (*pp)++;
                (*rest)--;
                if (s == 0x00) {
                    // NOP
                    evt->type = MMF_EVT_NOP;
                    evt->channel = 0;
                    return 0;
                }
                if (s == 0x2F) {
                    if (*rest < 1) return -1;
                    uint8_t s2 = **pp;
                    (*pp)++;
                    (*rest)--;
                    if (s2 == 0x00) return -2; // End of track
                }
                return -1; // Unknown meta event
            }
            return -1;
        }
        default:
            return -1;
    }
}

// Create event from HandyPhoneStandard (4-channel) format
static int create_event_hps(const uint8_t** pp, int* rest, MmfEvent* evt) {
    if (*rest <= 0) return -1;
    uint8_t sig = **pp;
    (*pp)++;
    (*rest)--;

    if (sig == 0xFF) {
        if (*rest < 1) return -1;
        uint8_t sig2 = **pp;
        (*pp)++;
        (*rest)--;
        if (sig2 == 0x00) {
            evt->type = MMF_EVT_NOP;
            evt->channel = 0;
            return 0;
        }
        if (sig2 == 0xF0) {
            evt->type = MMF_EVT_EXCLUSIVE;
            evt->channel = 0;
            MmfExclusive excl;
            if (read_exclusive(pp, rest, true, &excl) != 0) return -1;
            evt->exclusive.data = excl.data;
            evt->exclusive.data_len = excl.data_len;
            return 0;
        }
        return -1;
    }

    if (sig != 0) {
        // Note event
        int ch = sig >> 6;
        int oct = (sig >> 4) & 3;
        int note_val = sig & 15;
        int gate = read_variable_int(pp, rest, false);
        evt->type = MMF_EVT_NOTE;
        evt->channel = ch;
        evt->note.oct = oct;
        evt->note.note_val = note_val;
        evt->note.note = note_val + (oct + 3) * 12; // fallback for non-HPS paths
        evt->note.velocity = 127;
        evt->note.gate_time = gate;
        return 0;
    }

    // sig == 0: control event, read next byte
    if (*rest < 1) return -1;
    sig = **pp;
    (*pp)++;
    (*rest)--;

    int ch = sig >> 6;
    int type2 = (sig >> 4) & 3;

    if (type2 == 3) {
        // 1-parameter control: sig&0x0F = sub-type, next byte = value
        if (*rest < 1) return -1;
        uint8_t val = **pp;
        (*pp)++;
        (*rest)--;
        int v = val;
        switch (sig & 15) {
            case 0: // Program Change
                evt->type = MMF_EVT_PC;
                evt->channel = ch;
                evt->pc.pc = v;
                return 0;
            case 1: // Bank Select
                evt->type = MMF_EVT_CC;
                evt->channel = ch;
                evt->cc.cc = MMF_CC_BANK_SELECT_LSB;
                evt->cc.value = v;
                return 0;
            case 2: // Octave Shift
                if (v >= 0x80) v = 0x80 - v;
                evt->type = MMF_EVT_OCTAVE_SHIFT;
                evt->channel = ch;
                evt->octave_shift.value = v;
                return 0;
            case 3: // Modulation
                evt->type = MMF_EVT_CC;
                evt->channel = ch;
                evt->cc.cc = MMF_CC_MODULATION;
                evt->cc.value = v;
                return 0;
            case 4: // Pitch Bend
                evt->type = MMF_EVT_PITCHBEND;
                evt->channel = ch;
                evt->pitchbend.value = (v - 64) * (8192 / 64);
                return 0;
            case 7: // Volume
                evt->type = MMF_EVT_CC;
                evt->channel = ch;
                evt->cc.cc = MMF_CC_MAIN_VOLUME;
                evt->cc.value = v;
                return 0;
            case 10: // Panpot
                evt->type = MMF_EVT_CC;
                evt->channel = ch;
                evt->cc.cc = MMF_CC_PANPOT;
                evt->cc.value = v;
                return 0;
            case 11: // Expression
                evt->type = MMF_EVT_CC;
                evt->channel = ch;
                evt->cc.cc = MMF_CC_EXPRESSION;
                evt->cc.value = v;
                return 0;
            default:
                return -1;
        }
    } else if (type2 == 2) {
        // Modulation (short)
        evt->type = MMF_EVT_CC;
        evt->channel = ch;
        evt->cc.cc = MMF_CC_MODULATION;
        evt->cc.value = shortModTable[sig & 15];
        return 0;
    } else if (type2 == 1) {
        // Pitch Bend (short)
        evt->type = MMF_EVT_PITCHBEND;
        evt->channel = ch;
        evt->pitchbend.value = ((sig & 15) * 8 - 64) * (8192 / 64);
        return 0;
    } else {
        // Expression (short)
        evt->type = MMF_EVT_CC;
        evt->channel = ch;
        evt->cc.cc = MMF_CC_EXPRESSION;
        evt->cc.value = shortExpTable[sig & 15];
        return 0;
    }
}

// Create event from SEQU format
static int create_event_sequ(const uint8_t** pp, int* rest, MmfEvent* evt) {
    if (*rest <= 0) return -1;
    uint8_t sig = **pp;
    (*pp)++;
    (*rest)--;

    if (sig == 0x00) {
        if (*rest < 1) return -1;
        uint8_t sig2 = **pp;
        (*pp)++;
        (*rest)--;

        int ch = sig2 >> 6;
        int msg = sig2 & 0x3F;

        if (msg == 0x00) {
            // Fine tune
            if (*rest < 1) return -1;
            uint8_t fine = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_FINE_TUNE;
            evt->channel = ch;
            evt->fine_tune.value = fine;
            return 0;
        } else if (msg >= 0x01 && msg <= 0x0E) {
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_EXPRESSION;
            evt->cc.value = shortExpTable[msg];
            return 0;
        } else if (msg >= 0x11 && msg <= 0x1E) {
            evt->type = MMF_EVT_PITCHBEND;
            evt->channel = ch;
            evt->pitchbend.value = (msg - 0x10) * 16384 / 16;
            return 0;
        } else if (msg >= 0x21 && msg <= 0x2E) {
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_MODULATION;
            evt->cc.value = shortModTable[msg - 0x20];
            return 0;
        } else if (msg == 0x30) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_PC;
            evt->channel = ch;
            evt->pc.pc = val;
            return 0;
        } else if (msg == 0x31) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_BANK_SELECT_LSB;
            evt->cc.value = val;
            return 0;
        } else if (msg == 0x32) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            int v = val;
            if (v >= 0x80) v = 0x80 - v;
            evt->type = MMF_EVT_OCTAVE_SHIFT;
            evt->channel = ch;
            evt->octave_shift.value = v;
            return 0;
        } else if (msg == 0x33) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_MODULATION;
            evt->cc.value = val;
            return 0;
        } else if (msg == 0x34) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_PITCHBEND;
            evt->channel = ch;
            evt->pitchbend.value = val * 16384 / 256;
            return 0;
        } else if (msg == 0x36) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_EXPRESSION;
            evt->cc.value = val;
            return 0;
        } else if (msg == 0x37) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_MAIN_VOLUME;
            evt->cc.value = val;
            return 0;
        } else if (msg == 0x3A) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_PANPOT;
            evt->cc.value = val;
            return 0;
        } else if (msg == 0x3B) {
            if (*rest < 1) return -1;
            uint8_t val = **pp;
            (*pp)++;
            (*rest)--;
            evt->type = MMF_EVT_CC;
            evt->channel = ch;
            evt->cc.cc = MMF_CC_EXPRESSION;
            evt->cc.value = val;
            return 0;
        }
        return -1;
    } else if (sig == 0xFF) {
        if (*rest < 1) return -1;
        uint8_t sig2 = **pp;
        (*pp)++;
        (*rest)--;
        if (sig2 == 0x00) {
            evt->type = MMF_EVT_NOP;
            evt->channel = 0;
            return 0;
        }
        if (sig2 == 0xF0) {
            evt->type = MMF_EVT_EXCLUSIVE;
            evt->channel = 0;
            MmfExclusive excl;
            if (read_exclusive(pp, rest, false, &excl) != 0) return -1;
            evt->exclusive.data = excl.data;
            evt->exclusive.data_len = excl.data_len;
            return 0;
        }
        return -1;
    } else {
        // Note event
        int ch = sig >> 6;
        int oct = (sig >> 4) & 3;
        int note_val = sig & 15;
        int gate = read_variable_int(pp, rest, false);
        evt->type = MMF_EVT_NOTE;
        evt->channel = ch;
        evt->note.oct = oct;
        evt->note.note_val = note_val;
        evt->note.note = note_val + (oct + 3) * 12;
        evt->note.velocity = 127;
        evt->note.gate_time = gate;
        return 0;
    }
}

// ============ Parse sequence data chunk ============

static int parse_sequence_data(const uint8_t* data, int size, MmfFormatType format_type, MmfSequenceData* seq) {
    seq->format_type = format_type;
    memset(seq->is_channel_used, 0, sizeof(seq->is_channel_used));
    memset(seq->used_note_count, 0, sizeof(seq->used_note_count));
    seq->used_channel_count = 0;
    seq->used_pc_count = 0;
    memset(seq->channel_bank_msb, 0, sizeof(seq->channel_bank_msb));
    memset(seq->channel_bank_lsb, 0, sizeof(seq->channel_bank_lsb));
    memset(seq->channel_pc, 0, sizeof(seq->channel_pc));

    reset_velocity_context();

    const uint8_t* src = data;
    int rest = size;

    // For compressed format, decompress first
    uint8_t* decomp_buf = NULL;
    int decomp_size = 0;

    if (format_type == MMF_FMT_MOBILE_COMPRESSED) {
        // First 4 bytes = decompressed size (big-endian), rest = huffman data
        if (rest < 4) return -1;
        uint32_t out_size = read_u32_be(src);
        decomp_buf = (uint8_t*)malloc(out_size);
        if (!decomp_buf) return -1;

        decomp_size = huffman_decode(src, rest, decomp_buf, out_size);
        if (decomp_size <= 0) {
            free(decomp_buf);
            return -1;
        }

        src = decomp_buf;
        rest = decomp_size;
    }

    // Read events
    int parse_errors = 0;
    while (rest > 0) {
        // Check for end-of-sequence marker (4 zero bytes)
        if (rest == 4) {
            uint32_t eos = read_u32_be(src);
            if (eos == 0) break;
        }

        MmfEvent evt;
        memset(&evt, 0, sizeof(evt));

        int allow3 = (format_type == MMF_FMT_MOBILE_NORMAL || format_type == MMF_FMT_MOBILE_COMPRESSED);
        int dur = read_variable_int(&src, &rest, allow3);
        evt.duration_ms = dur;

        int result;
        switch (format_type) {
            case MMF_FMT_HPS:
                result = create_event_hps(&src, &rest, &evt);
                break;
            case MMF_FMT_SEQU:
                result = create_event_sequ(&src, &rest, &evt);
                break;
            case MMF_FMT_MOBILE_NORMAL:
            case MMF_FMT_MOBILE_COMPRESSED:
                result = create_event_mobile(&src, &rest, &evt);
                break;
            default:
                result = -1;
                break;
        }

        if (result == -2) break; // End of track
        if (result < 0) {
            continue;
        }

        // Track channel usage and PC state
        if (evt.type == MMF_EVT_CC && evt.cc.cc == MMF_CC_BANK_SELECT_MSB) {
            seq->channel_bank_msb[evt.channel] = evt.cc.value;
        } else if (evt.type == MMF_EVT_CC && evt.cc.cc == MMF_CC_BANK_SELECT_LSB) {
            seq->channel_bank_lsb[evt.channel] = evt.cc.value;
        } else if (evt.type == MMF_EVT_PC) {
            seq->channel_pc[evt.channel] = evt.pc.pc;
        } else if (evt.type == MMF_EVT_NOTE) {
            if (!seq->is_channel_used[evt.channel]) {
                seq->is_channel_used[evt.channel] = 1;
            }
            seq->used_note_count[evt.channel]++;

            // Track used PC
            uint32_t pc_key = ((uint32_t)seq->channel_bank_msb[evt.channel] << 24) |
                              ((uint32_t)seq->channel_bank_lsb[evt.channel] << 16) |
                              ((uint32_t)seq->channel_pc[evt.channel] << 8);
            // Add to used_pc set if not already there
            int found = 0;
            for (int i = 0; i < seq->used_pc_count; i++) {
                if (seq->used_pc[i] == pc_key) { found = 1; break; }
            }
            if (!found && seq->used_pc_count < 256) {
                seq->used_pc[seq->used_pc_count++] = pc_key;
            }
        }

        seq->events.push_back(evt);
    }

    // Count used channels
    for (int i = 0; i < 16; i++) {
        if (seq->is_channel_used[i]) seq->used_channel_count++;
    }

    if (decomp_buf) free(decomp_buf);
    return 0;
}

// ============ Parse CNTI (Contents Info) chunk ============

static int parse_cnti(const uint8_t* data, int size, MmfContentsInfo* cnti) {
    memset(cnti, 0, sizeof(*cnti));

    if (size < 5) return -1;

    cnti->contents_class = data[0];
    cnti->contents_type = data[1];
    cnti->contents_code_type = data[2];
    cnti->copy_status = data[3];
    cnti->copy_counts = data[4];

    cnti->stream = data + 5;
    cnti->stream_len = size - 5;

    // Decode optional fields if Shift-JIS text
    if (cnti->contents_code_type == 0x00) {
        cnti->has_options = 1;
        mmf_decode_optional_fields(cnti->stream, cnti->stream_len,
            cnti->vendor, sizeof(cnti->vendor),
            cnti->carrier, sizeof(cnti->carrier),
            cnti->category, sizeof(cnti->category),
            cnti->title, sizeof(cnti->title),
            cnti->artist, sizeof(cnti->artist),
            cnti->composer, sizeof(cnti->composer));
    }

    return 0;
}

// ============ Parse optional data fields (tag-value pairs) ============

int mmf_decode_optional_fields(const uint8_t* stream, int stream_len,
                                char* vendor, int vlen,
                                char* carrier, int clen,
                                char* category, int catlen,
                                char* title, int tlen,
                                char* artist, int alen,
                                char* composer, int swlen) {
    if (vendor) vendor[0] = '\0';
    if (carrier) carrier[0] = '\0';
    if (category) category[0] = '\0';
    if (title) title[0] = '\0';
    if (artist) artist[0] = '\0';
    if (composer) composer[0] = '\0';

    int i = 0;
    while (i + 3 < stream_len) {
        char tag[3] = { (char)stream[i], (char)stream[i+1], '\0' };
        i += 2;
        int sz = (stream[i] << 8) | stream[i+1];
        i += 2;

        if (i + sz > stream_len) break;

        char buf[256];
        int blen = (sz < 255) ? sz : 255;
        sjis_to_utf8(stream + i, blen, buf, sizeof(buf));

        if (strcmp(tag, "VN") == 0 && vendor) strncpy(vendor, buf, vlen-1);
        else if (strcmp(tag, "CN") == 0 && carrier) strncpy(carrier, buf, clen-1);
        else if (strcmp(tag, "CA") == 0 && category) strncpy(category, buf, catlen-1);
        else if (strcmp(tag, "ST") == 0 && title) strncpy(title, buf, tlen-1);
        else if (strcmp(tag, "AN") == 0 && artist) strncpy(artist, buf, alen-1);
        else if (strcmp(tag, "SW") == 0 && composer) strncpy(composer, buf, swlen-1);

        i += sz;
    }
    return 0;
}

// ============ MA version detection ============

int mmf_detect_version_from_cnti(const MmfContentsInfo* cnti) {
    // Method 1: Look for "Mx:" pattern in decoded options
    // The stream field contains optional data like "VN...\0ST...Title\0"
    // Some files have version info embedded in the stream or options

    // Search raw stream for "Mx:" pattern
    const uint8_t* s = cnti->stream;
    int slen = cnti->stream_len;

    for (int i = 0; i + 2 < slen; i++) {
        if (s[i] == 'M' && s[i+1] >= '1' && s[i+1] <= '7' && s[i+2] == ':') {
            return s[i+1] - '0';
        }
    }

    // Also check decoded text fields
    const char* fields[] = { cnti->vendor, cnti->carrier, cnti->category,
                             cnti->title, cnti->artist, cnti->composer };

    for (int f = 0; f < 6; f++) {
        if (!fields[f][0]) continue;
        for (int i = 0; fields[f][i]; i++) {
            if (fields[f][i] == 'M' && fields[f][i+1] >= '1' && fields[f][i+1] <= '7' && fields[f][i+2] == ':') {
                return fields[f][i+1] - '0';
            }
        }
    }

    return 0; // Unknown
}

int mmf_detect_version_from_exclusives(const MmfExclusive* exclusives, int count) {
    // Check exclusives for known Yamaha SysEx patterns
    // MA-5: 43 79 07 7F 01 ...
    // MA-3: 43 79 06 7F 01 ...
    // MA-5 simplified: 43 05 01 ...
    // MA-2: 43 03 ...

    // Priority: check all exclusives, return first match
    // Later matches (more specific patterns) take priority if multiple found
    int found_version = 0;

    for (int i = 0; i < count; i++) {
        const MmfExclusive* e = &exclusives[i];
        const uint8_t* d = e->data;
        int dlen = e->data_len;

        if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x07 && d[3] == 0x7F && d[4] == 0x01) {
            found_version = 5; // MA-5 (most specific, keep)
        } else if (dlen >= 5 && d[0] == 0x43 && d[1] == 0x79 && d[2] == 0x06 && d[3] == 0x7F && d[4] == 0x01) {
            if (found_version != 5) found_version = 3; // MA-3
        } else if (dlen >= 3 && d[0] == 0x43 && d[1] == 0x05 && d[2] == 0x01) {
            if (found_version != 5 && found_version != 3) found_version = 5; // MA-5 simplified
        } else if (dlen >= 2 && d[0] == 0x43 && d[1] == 0x03) {
            if (found_version == 0) found_version = 2; // MA-2 (least specific, only if nothing else)
        } else if (dlen >= 2 && d[0] == 0x43 && d[1] == 0x02) {
            if (found_version == 0) found_version = 2; // MA-1/2 native voice (Panasonic G60/X300)
        }
    }

    return found_version;
}

// ============ Parse a ScoreTrack (MTR*) chunk ============

static int parse_score_track(const uint8_t* data, int size, MmfScoreTrack* track,
                              MmfSequenceData* seq, MmfSetupData* setup) {
    mmf_log("[MMF] parse_score_track: size=%d\n", size);
    memset(track, 0, sizeof(*track));
    seq->events.clear();
    setup->exclusives.clear();

    if (size < 4) return -1;

    track->format_type = (MmfFormatType)data[0];
    track->sequence_type = data[1];
    track->duration_time_base_ms = time_base_from_byte(data[2]);
    track->gate_time_base_ms = time_base_from_byte(data[3]);

    const uint8_t* p = data + 4;
    int rest = size - 4;

    // Parse channel status
    memset(track->channel_status, 0, sizeof(track->channel_status));

    if (track->format_type == MMF_FMT_HPS) {
        // 4 channels, packed in 2 bytes
        if (rest < 2) return -1;
        uint16_t b = read_u16_be(p);
        p += 2;
        rest -= 2;
        for (int ch = 0; ch < 4; ch++) {
            track->channel_status[ch].key_control_status = (b >> 15 & 1) + 1;
            track->channel_status[ch].vibration_status = (b >> 14 & 1) != 0;
            track->channel_status[ch].channel_type = b >> 12 & 3;
            b <<= 4;
        }
    } else {
        // 16 channels, 1 byte each
        for (int ch = 0; ch < 16 && rest > 0; ch++) {
            uint8_t b = *p++;
            rest--;
            track->channel_status[ch].key_control_status = b >> 6;
            track->channel_status[ch].vibration_status = (b >> 5) & 1;
            track->channel_status[ch].led_status = (b >> 4) & 1;
            track->channel_status[ch].channel_type = b & 3;
        }
    }

    // Parse sub-chunks (Mtsq, Mtsu, EXVO, etc.)
    while (rest >= 8) {
        uint32_t sig = read_u32_be(p);
        uint32_t chunk_size = read_u32_be(p + 4);
        p += 8;
        rest -= 8;

        mmf_log("[MMF]   sub-chunk: sig=0x%08X size=%u rest=%d\n", sig, chunk_size, rest);

        if (chunk_size > (uint32_t)rest) {
            mmf_log("[MMF]   chunk_size > rest, break\n");
            break;
        }

        if (sig == MMF_SIG_MTSQ) {
            // Sequence data
            mmf_log("[MMF]   Mtsq: format=%d chunk_size=%u\n", track->format_type, chunk_size);
            parse_sequence_data(p, chunk_size, track->format_type, seq);
            mmf_log("[MMF]   Mtsq result: %d events, %d channels\n",
                    (int)seq->events.size(), seq->used_channel_count);
            for (int ci = 0; ci < 16; ci++) {
                if (seq->is_channel_used[ci])
                    mmf_log("[MMF]     ch%d: %d notes\n", ci, seq->used_note_count[ci]);
            }
        } else if (sig == MMF_SIG_MTSU) {
            // Setup data - per smaf825-1 ScoreTrackSetupDataChunk.Read()
            const uint8_t* sp = p;
            int srest = chunk_size;
            while (srest >= 1) {
                uint8_t s = *sp++; srest--;
                if (s == 0xFF) {
                    if (srest < 1) break;
                    s = *sp++; srest--;
                }
                if (s == 0xF0) {
                    MmfExclusive excl;
                    if (read_exclusive(&sp, &srest, false, &excl) == 0) {
                        setup->exclusives.push_back(excl);
                    }
                } else {
                    break;
                }
            }
        } else if (sig == MMF_SIG_EXVO) {
            MmfExclusive excl;
            if (read_exclusive(&p, &rest, true, &excl) == 0) {
                setup->exclusives.push_back(excl);
            }
            rest -= chunk_size;  // read_exclusive already moved p, but rest needs aligning
            p = p; // p already advanced by read_exclusive
            continue;  // skip the p += chunk_size and rest -= chunk_size below
        } else {
            // unknown sub-chunk
        }
        p += chunk_size;
        rest -= chunk_size;
    }

    return 0;
}

// ============ Parse ATR (Audio Track) chunk ============

static int get_flex_len(const uint8_t* p, int avail) {
    if (avail < 1) return -1;
    if (p[0] < 0x80) return p[0];
    if (avail < 2) return -1;
    return ((p[0] & 0x7F) << 7) + (p[1] & 0x7F) + 128;
}

static int parse_atr(const uint8_t* data, int size, MmfFile* out) {
    mmf_log("[MMF] parse_atr: size=%d\n", size);

    if (size < 6) return -1;

    // ATR header: format_type, seq_type, wave_type, reserved, timebase, timebase
    uint8_t wave_type = data[2];
    if (data[0] != 0x00 || data[1] != 0x00 || (data[3] & 0xF0) != 0x00) {
        mmf_log("[MMF] ATR header check failed\n");
        return -1;
    }
    if (data[4] != data[5]) {
        mmf_log("[MMF] ATR timebase mismatch\n");
        return -1;
    }

    uint32_t sample_rate = 0;
    switch (wave_type) {
        case 0x10: sample_rate = 4000; break;
        case 0x11: sample_rate = 8000; break;
        default:
            mmf_log("[MMF] ATR unsupported wave_type: 0x%02X\n", wave_type);
            return -1;
    }

    int timebase_ms = 0;
    switch (data[4]) {
        case 1: timebase_ms = 1; break;
        case 2: timebase_ms = 2; break;
        case 4: timebase_ms = 4; break;
        case 5: timebase_ms = 5; break;
        default: timebase_ms = 10; break;
    }

    out->has_atr = 1;
    out->atr_wave_type = wave_type;
    out->atr_timebase_ms = timebase_ms;
    out->atr_wave_data = NULL;
    out->atr_wave_size = 0;
    out->atr_st_tick = 0;
    out->atr_sp_tick = 0;

    mmf_log("[MMF] ATR: wave_type=0x%02X (%dHz), timebase=%dms\n", wave_type, sample_rate, timebase_ms);

    // Scan sub-chunks
    const uint8_t* p = data + 6;
    int rest = size - 6;

    while (rest >= 8) {
        uint32_t sig = read_u32_be(p);
        uint32_t csz = read_u32_be(p + 4);
        p += 8;
        rest -= 8;
        if (csz > (uint32_t)rest) break;

        if (sig == 0x41737049) { // "AspI"
            // Parse tag-length-value: "st:" + 4B value, "sp:" + 4B value
            const uint8_t* ap = p;
            int arest = csz;
            while (arest >= 8) {
                uint16_t tag = (ap[0] << 8) | ap[1];
                ap += 3; // skip tag + separator
                uint32_t val = (ap[0] << 24) | (ap[1] << 16) | (ap[2] << 8) | ap[3];
                ap += 4; // skip value
                ap += 1; // skip comma
                arest -= 8;
                if (tag == 0x7374) { // "st"
                    out->atr_st_tick = val;
                    mmf_log("[MMF] ATR st=%d\n", val);
                } else if (tag == 0x7370) { // "sp"
                    out->atr_sp_tick = val;
                    mmf_log("[MMF] ATR sp=%d\n", val);
                }
            }
        } else if (sig == 0x41747371) { // "Atsq"
            // Sequence data - same format as Mtsq (HPS)
            out->atr_sequence.events.clear();
            parse_sequence_data(p, csz, MMF_FMT_HPS, &out->atr_sequence);
            mmf_log("[MMF] ATR Atsq: %d events\n", (int)out->atr_sequence.events.size());
        } else if ((sig & 0xFFFFFF00) == 0x41776100) { // "Awa*" - Wave data
            out->atr_wave_data = p;
            out->atr_wave_size = csz;
            mmf_log("[MMF] ATR Awa: %d bytes\n", csz);
        }

        p += csz;
        rest -= csz;
    }

    return 0;
}

// ============ iMelody (.imy) parser ============
// Spec: EMS iMelody 1.2 (reference/imelody-editor "iMelody format instruction
// (ems_imelody).txt", imyplay). Monophonic text format. Emitted as a single
// 1ms-timebase track of MIDI note/CC events; MA-2 engine renders it.

static const char* imy_find(const uint8_t* data, int size, const char* needle) {
    int n = (int)strlen(needle);
    for (int i = 0; i + n <= size; i++)
        if (memcmp(data + i, needle, n) == 0) return (const char*)data + i;
    return NULL;
}

static int imy_parse_melody(const char* s, const char* end, int beat,
                            int style, MmfSequenceData* seq) {
    // duration codes 0..5 = whole,1/2,1/4,1/8,1/16,1/32 notes; beat = quarter bpm
    static const double durBeats[6] = {4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
    // S0 natural 20:1, S1 continuous (full), S2 staccato 1:1
    double styleRatio = style == 1 ? 1.0 : (style == 2 ? 0.5 : 0.95);
    double quarterMs = 60000.0 / (beat > 0 ? beat : 120);
    int volume = 7;

    int octave = 4; // default *4 (A=880Hz)
    int pendingDur = 2; // default 1/4 note

    const char* p = s;
    while (p < end) {
        char c = *p;
        if (c == '(') {
            // repeat block: "(...)@count" — no nesting per spec
            const char* q = ++p;
            while (q < end && *q != ')') q++;
            if (q >= end || q + 1 >= end || q[1] != '@') { p = q < end ? q + 1 : end; continue; }
            const char* r = q + 2;
            int count = 0;
            while (r < end && *r >= '0' && *r <= '9') { count = count * 10 + (*r - '0'); r++; }
            // count 0 = forever; cap at 64 to avoid infinite data
            if (count == 0) count = 64;
            for (int i = 0; i < count; i++)
                imy_parse_melody(p, q, beat, style, seq); // volume carries via local copies
            p = r;
            continue;
        }
        if (c == '*') { // octave prefix
            p++;
            int v = 0;
            while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (v >= 0 && v <= 8) octave = v;
            continue;
        }
        if (c == 'V') { // volume: V+n / V-n / Vn
            p++;
            if (p < end && (*p == '+' || *p == '-')) {
                int sign = (*p == '+') ? 1 : -1;
                p++;
                int v = 0;
                while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                volume += sign * (v > 0 ? v : 1);
            } else {
                int v = 0;
                while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                volume = v;
            }
            if (volume < 0) volume = 0;
            if (volume > 15) volume = 15;
            MmfEvent evt; memset(&evt, 0, sizeof(evt));
            evt.duration_ms = 0;
            evt.type = MMF_EVT_CC; evt.channel = 0;
            evt.cc.cc = MMF_CC_MAIN_VOLUME;
            evt.cc.value = volume * 127 / 15;
            seq->events.push_back(evt);
            continue;
        }
        if (c == 'r') { // rest
            p++;
            int d = 2;
            while (p < end && *p >= '0' && *p <= '9') { d = *p - '0'; p++; }
            double mult = 1.0;
            if (p < end && *p == '.') { mult = 1.5; p++; }
            else if (p < end && *p == ':') { mult = 1.75; p++; }
            else if (p < end && *p == ';') { mult = 2.0 / 3.0; p++; }
            MmfEvent evt; memset(&evt, 0, sizeof(evt));
            evt.duration_ms = (int)(durBeats[d] * quarterMs * mult + 0.5);
            evt.type = MMF_EVT_NOP; evt.channel = 0;
            seq->events.push_back(evt);
            continue;
        }
        if ((c >= 'a' && c <= 'g') || c == '#' || c == '&') {
            int accidental = 0;
            if (c == '#') { accidental = 1; p++; }
            else if (c == '&') { accidental = -1; p++; }
            if (p >= end) break;
            char letter = *p++;
            if (letter < 'a' || letter > 'g') continue;
            static const int semitoneByLetter[7] = {9, 11, 0, 2, 4, 5, 7}; // indexed by letter a..b
            // MIDI: octave 4 c = 60 (C4), i.e. 12*(octave+1)
            int midi = 12 * (octave + 1) + semitoneByLetter[letter - 'a'] + accidental;
            if (midi < 0) midi = 0;
            if (midi > 127) midi = 127;
            int d = 2;
            while (p < end && *p >= '0' && *p <= '9') { d = *p - '0'; p++; }
            double mult = 1.0;
            if (p < end && *p == '.') { mult = 1.5; p++; }
            else if (p < end && *p == ':') { mult = 1.75; p++; }
            else if (p < end && *p == ';') { mult = 2.0 / 3.0; p++; }
            int totalMs = (int)(durBeats[d] * quarterMs * mult + 0.5);
            MmfEvent evt; memset(&evt, 0, sizeof(evt));
            evt.duration_ms = 0;
            evt.type = MMF_EVT_NOTE; evt.channel = 0;
            evt.note.note = midi;
            evt.note.note_val = midi; // same value so any fmt path works
            evt.note.velocity = 127;
            evt.note.gate_time = (int)(totalMs * styleRatio + 0.5);
            seq->events.push_back(evt);
            MmfEvent off; memset(&off, 0, sizeof(off));
            off.duration_ms = totalMs;
            off.type = MMF_EVT_NOP; off.channel = 0;
            seq->events.push_back(off);
            continue;
        }
        p++; // ledon/vibeon/backon etc: skip unknown chars
    }
    return 0;
}

static int parse_imelody(const uint8_t* data, int size, MmfFile* out) {
    const char* begin = imy_find(data, size, "BEGIN:IMELODY");
    if (!begin) return -1;
    const char* text = begin;
    const char* end = (const char*)data + size;

    // Header fields
    int beat = 120, style = 0;
    const char* ml = imy_find((const uint8_t*)text, (int)(end - text), "MELODY:");
    if (!ml) return -1;
    for (const char* q = text; q < ml; ) {
        const char* eol = q;
        while (eol < end && *eol != '\n' && *eol != '\r') eol++;
        if (eol - q > 5 && memcmp(q, "BEAT:", 5) == 0) {
            beat = atoi(q + 5);
        } else if (eol - q > 6 && memcmp(q, "STYLE:", 6) == 0) {
            const char* v = q + 6;
            while (v < eol && (*v < '0' || *v > '9')) v++;
            style = (v < eol) ? (*v - '0') : 0;
        }
        q = (*eol == '\r' && eol + 1 < end) ? eol + 2 : eol + 1;
    }

    out->tracks = new MmfScoreTrack[1]();
    out->sequences = new MmfSequenceData[1];
    out->setups = new MmfSetupData[1];
    out->track_count = 1;
    out->tracks[0].format_type = MMF_FMT_MOBILE_NORMAL;
    out->sequences[0].format_type = MMF_FMT_MOBILE_NORMAL;
    out->tracks[0].duration_time_base_ms = 1; // events already in ms
    out->tracks[0].gate_time_base_ms = 1;
    out->sequences[0].events.clear();

    const char* melody = ml + 7;
    const char* mend = melody;
    while (mend + 11 <= end) {
        if (memcmp(mend, "END:IMELODY", 11) == 0) break;
        if (*mend == '\n' || *mend == '\r') break; // melody is one line
        mend++;
    }
    imy_parse_melody(melody, mend, beat, style, &out->sequences[0]);

    out->sequences[0].is_channel_used[0] = true;
    out->sequences[0].used_channel_count = 1;
    out->sequences[0].used_note_count[0] =
        (int)out->sequences[0].events.size() / 2;
    out->detected_ma_version = 2; // monophonic beep era -> MA-2 engine
    strcpy(out->detected_ma_name, "MA-2 (iMelody)");
    mmf_log("[MMF] iMelody: beat=%d style=%d, %d events\n",
            beat, style, (int)out->sequences[0].events.size());
    return 0;
}

// ============ Main parse function ============

int mmf_parse(const uint8_t* data, int size, MmfFile* out) {
    *out = MmfFile();
    out->raw = data;
    out->raw_len = size;
    out->detected_ma_version = 0;
    strcpy(out->detected_ma_name, "Unknown");

    if (size < 12) {
        snprintf(out->error, sizeof(out->error), "File too small (%d bytes)", size);
        // Raw .imy text can still be tried (a bare melody line is longer than 12 bytes)
        if (size > 12 && imy_find(data, size, "BEGIN:IMELODY"))
            return parse_imelody(data, size, out);
        return -1;
    }

    // Check MMF signature
    if (read_u32_be(data) != 0x4D4D4D44) { // "MMMD"
        // Raw iMelody text (.imy)
        if (imy_find(data, size, "BEGIN:IMELODY"))
            return parse_imelody(data, size, out);
        snprintf(out->error, sizeof(out->error), "Not an MMF file (signature: 0x%08X)", read_u32_be(data));
        return -1;
    }

    // uint32_t file_size = read_u32_be(data + 4); // available if needed
    const uint8_t* p = data + 8;
    int rest = size - 8;

    // First pass: count tracks (also recurse into MMMG)
    int track_count = 0;
    const uint8_t* scan = p;
    int scan_rest = rest - 2; // minus CRC
    while (scan_rest >= 8) {
        uint32_t sig = read_u32_be(scan);
        uint32_t csz = read_u32_be(scan + 4);
        scan += 8;
        scan_rest -= 8;
        if (csz > (uint32_t)scan_rest) break;

        if ((sig & 0xFFFFFF00) == MMF_SIG_MTR) {
            track_count++;
        } else if (sig == MMF_SIG_MMMG) {
            // MMMG has 2-byte enigma, then sub-chunks
            const uint8_t* mp = scan + 2;
            int mrest = csz - 2;
            while (mrest >= 8) {
                uint32_t msig = read_u32_be(mp);
                uint32_t mcsz = read_u32_be(mp + 4);
                mp += 8;
                mrest -= 8;
                if (mcsz > (uint32_t)mrest) break;
                if ((msig & 0xFFFFFF00) == MMF_SIG_MTR) {
                    track_count++;
                } else if (msig == MMF_SIG_SEQU) {
                    // SoftBank/Sharp MMMG phrase sequence (ReMEXA formatType 0x04)
                    track_count++;
                }
                mp += mcsz;
                mrest -= mcsz;
            }
        }
        scan += csz;
        scan_rest -= csz;
    }

    if (track_count == 0) {
        // Check for SEQU chunk directly (no MMMG wrapper)
        int has_seq = 0;
        scan = p;
        scan_rest = rest - 2;
        while (scan_rest >= 8) {
            uint32_t sig = read_u32_be(scan);
            uint32_t csz = read_u32_be(scan + 4);
            if (sig == MMF_SIG_SEQU) has_seq = 1;
            scan += 8;
            if (csz > (uint32_t)(scan_rest - 8)) break;
            scan += csz;
            scan_rest -= csz - 8;
        }
        if (has_seq) track_count = 1;
    }

    // Allocate track arrays
    if (track_count > 0) {
        out->tracks = new MmfScoreTrack[track_count]();
        out->sequences = new MmfSequenceData[track_count];
        out->setups = new MmfSetupData[track_count];
        out->track_count = track_count;
    }

    // Second pass: parse chunks
    int track_idx = 0;
    while (rest >= 8) {
        uint32_t sig = read_u32_be(p);
        uint32_t csz = read_u32_be(p + 4);
        p += 8;
        rest -= 8;

        if (csz > (uint32_t)rest) break;

        if (sig == MMF_SIG_CNTI) {
            parse_cnti(p, csz, &out->cnti);
            out->has_cnti = 1;
        } else if ((sig & 0xFFFFFF00) == MMF_SIG_MTR) {
            // Score track
            if (track_idx < track_count) {
                parse_score_track(p, csz, &out->tracks[track_idx],
                                  &out->sequences[track_idx],
                                  &out->setups[track_idx]);
                track_idx++;
            }
        } else if (sig == MMF_SIG_SEQU) {
            // Standalone SEQU chunk (no MMMG wrapper)
            if (track_idx < track_count) {
                out->tracks[track_idx].format_type = MMF_FMT_SEQU;
                out->tracks[track_idx].duration_time_base_ms = 1;
                out->tracks[track_idx].gate_time_base_ms = 1;
                parse_sequence_data(p, csz, MMF_FMT_SEQU, &out->sequences[track_idx]);
                track_idx++;
            }
        } else if ((sig & 0xFFFFFF00) == MMF_SIG_ATR) {
            parse_atr(p, csz, out);
        } else if (sig == MMF_SIG_MMMG) {
            // MMMG container - 2-byte header (sequenceHeader, literal timebase ms),
            // then VOIC/SEQU/MTR* sub-chunks (SoftBank/Sharp phrase format,
            // per ReMEXA decodeMMMGChunk / smaf825-1 MMMGChunk)
            out->has_mmmg = 1;
            const uint8_t* mp = p;
            int mrest = csz;
            int mmmgTimebaseMs = 20; // default 20ms per ReMEXA (Sharp/SoftBank files use 0x14)
            if (mrest >= 2) {
                // mp[0] = sequenceHeader (bit0 should be set), mp[1] = literal ms timebase
                if (mp[1] > 0 && mp[1] <= 250) mmmgTimebaseMs = mp[1];
                mp += 2;
                mrest -= 2;
            }
            while (mrest >= 8) {
                uint32_t msig = read_u32_be(mp);
                uint32_t mcsz = read_u32_be(mp + 4);
                mp += 8;
                mrest -= 8;
                if (mcsz > (uint32_t)mrest) break;

                if ((msig & 0xFFFFFF00) == MMF_SIG_MTR) {
                    if (track_idx < track_count) {
                        parse_score_track(mp, mcsz, &out->tracks[track_idx],
                                          &out->sequences[track_idx],
                                          &out->setups[track_idx]);
                        track_idx++;
                    }
                } else if (msig == MMF_SIG_SEQU) {
                    // Phrase sequence data: SEQU event format, literal-ms timebase
                    if (track_idx < track_count) {
                        out->tracks[track_idx].format_type = MMF_FMT_SEQU;
                        out->tracks[track_idx].duration_time_base_ms = mmmgTimebaseMs;
                        out->tracks[track_idx].gate_time_base_ms = mmmgTimebaseMs;
                        parse_sequence_data(mp, mcsz, MMF_FMT_SEQU, &out->sequences[track_idx]);
                        mmf_log("[MMF] MMMG > SEQU: tb=%dms, %d events\n",
                                mmmgTimebaseMs, (int)out->sequences[track_idx].events.size());
                        track_idx++;
                    }
                } else if (msig == 0x564F4943 /* "VOIC" */) {
                    // Voice container: walk sub-chunks; EXVO = exclusive voice,
                    // DEVO = device voice (built-in, no data to collect)
                    const uint8_t* vp = mp;
                    int vrest = mcsz;
                    while (vrest >= 8) {
                        uint32_t vsig = read_u32_be(vp);
                        uint32_t vcsz = read_u32_be(vp + 4);
                        vp += 8;
                        vrest -= 8;
                        if (vcsz > (uint32_t)vrest) break;
                        if (vsig == MMF_SIG_EXVO) {
                            MmfExclusive excl;
                            const uint8_t* ep = vp;
                            int erest = vrest;
                            if (read_exclusive(&ep, &erest, true, &excl) == 0) {
                                out->all_exclusives.push_back(excl);
                            }
                        }
                        vp += vcsz;
                        vrest -= vcsz;
                    }
                } else if (msig == MMF_SIG_EXVO) {
                    MmfExclusive excl;
                    const uint8_t* ep = mp;
                    int erest = mrest;
                    if (read_exclusive(&ep, &erest, true, &excl) == 0) {
                        out->all_exclusives.push_back(excl);
                    }
                }
                mp += mcsz;
                mrest -= mcsz;
            }
        }

        p += csz;
        rest -= csz;
    }

    // Collect all exclusives from all tracks
    for (int i = 0; i < out->track_count; i++) {
        for (const auto& excl : out->setups[i].exclusives) {
            out->all_exclusives.push_back(excl);
        }
    }

    // Detect MA version
    // Priority 1: CNTI text
    int ver = mmf_detect_version_from_cnti(&out->cnti);
    if (ver > 0) {
        out->detected_ma_version = ver;
    }

    // Priority 2: Exclusive voice data
    if (ver == 0 && !out->all_exclusives.empty()) {
        ver = mmf_detect_version_from_exclusives(
            out->all_exclusives.data(), out->all_exclusives.size());
        if (ver > 0) out->detected_ma_version = ver;
    }

    // Priority 3: HPS (fmt 0) / SEQU phrase files are Handy Phone family → MA-2
    if (out->detected_ma_version == 0) {
        for (int i = 0; i < out->track_count; i++) {
            if (out->tracks[i].format_type == MMF_FMT_HPS ||
                out->tracks[i].format_type == MMF_FMT_SEQU) {
                out->detected_ma_version = 2;
                break;
            }
        }
    }
    if (out->detected_ma_version == 0) {
        // Mobile Standard (fmt 1/2/3) with no version markers → MA-3
        for (int i = 0; i < out->track_count; i++) {
            if (out->tracks[i].format_type == MMF_FMT_MOBILE_NORMAL ||
                out->tracks[i].format_type == MMF_FMT_MOBILE_COMPRESSED) {
                out->detected_ma_version = 3;
                break;
            }
        }
    }

    // Set name
    switch (out->detected_ma_version) {
        case 1: strcpy(out->detected_ma_name, "MA-1"); break;
        case 2: strcpy(out->detected_ma_name, "MA-2"); break;
        case 3: strcpy(out->detected_ma_name, "MA-3"); break;
        case 5: strcpy(out->detected_ma_name, "MA-5"); break;
        case 7: strcpy(out->detected_ma_name, "MA-7"); break;
        default: strcpy(out->detected_ma_name, "Unknown"); break;
    }

    return 0;
}

void mmf_free(MmfFile* f) {
    if (f->tracks) { delete[] f->tracks; f->tracks = NULL; }
    if (f->sequences) { delete[] f->sequences; f->sequences = NULL; }
    if (f->setups) { delete[] f->setups; f->setups = NULL; }
    f->all_exclusives.clear();
    f->track_count = 0;
}
