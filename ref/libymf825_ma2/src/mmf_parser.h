/**
 * C++ MMF/SMAF Parser
 * Based on smaf825-1 (github.com/but80/smaf825)
 * Parses MMF chunk structure and sequence events for visualization
 */

#ifndef MMF_PARSER_H
#define MMF_PARSER_H

#include <stdint.h>
#include <vector>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif

// ============ Chunk signatures ============
#define MMF_SIG_CNTI  0x434E5449  // "CNTI"
#define MMF_SIG_OPDA  0x4F504441  // "OPDA"
#define MMF_SIG_MMMG  0x4D4D4D47  // "MMMG"
#define MMF_SIG_MTSQ  0x4D747371  // "Mtsq"
#define MMF_SIG_MTSU  0x4D747375  // "Mtsu"
#define MMF_SIG_SEQU  0x53455155  // "SEQU"
#define MMF_SIG_EXVO  0x4558564F  // "EXVO"
#define MMF_SIG_VOIC  0x564F4943  // "VOIC"
#define MMF_SIG_MSPI  0x4D737049  // "MspI"
#define MMF_SIG_MTR   0x4D545200  // "MTR*" (base, last byte varies)
#define MMF_SIG_DCH   0x44636800  // "Dch*" (base, last byte varies)
#define MMF_SIG_ATR   0x41545200  // "ATR*" (Audio Track)

// ============ Format types ============
enum MmfFormatType {
    MMF_FMT_HPS = 0,           // HandyPhone Standard (4 channels)
    MMF_FMT_MOBILE_COMPRESSED, // Mobile Standard Compressed (16ch, Huffman)
    MMF_FMT_MOBILE_NORMAL,     // Mobile Standard Non-Compressed (16ch)
    MMF_FMT_SEQU = -1,         // SEQU format
};

// ============ Channel status ============
enum MmfChannelType {
    MMF_CHTYPE_FM  = 0,
    MMF_CHTYPE_PCM = 1,
    MMF_CHTYPE_STREAM = 2,
    MMF_CHTYPE_INVALID = 3,
};

typedef struct {
    int key_control_status;
    int vibration_status;
    int led_status;
    int channel_type; // MmfChannelType
} MmfChannelStatus;

// ============ Event types ============
enum MmfEventType {
    MMF_EVT_NOTE = 0,
    MMF_EVT_CC,
    MMF_EVT_PC,
    MMF_EVT_PITCHBEND,
    MMF_EVT_OCTAVE_SHIFT,
    MMF_EVT_FINE_TUNE,
    MMF_EVT_EXCLUSIVE,
    MMF_EVT_NOP,
};

enum MmfCCType {
    MMF_CC_BANK_SELECT_MSB = 0,
    MMF_CC_MODULATION = 1,
    MMF_CC_MAIN_VOLUME = 7,
    MMF_CC_PANPOT = 10,
    MMF_CC_EXPRESSION = 11,
    MMF_CC_BANK_SELECT_LSB = 32,
};

typedef struct {
    int duration_ms;    // Time from previous event (ms, before timebase applied)
    MmfEventType type;
    int channel;        // 0-15

    union {
        struct { int note; int velocity; int gate_time; int oct; int note_val; } note;
        struct { int cc; int value; } cc;
        struct { int pc; } pc;
        struct { int value; } pitchbend;
        struct { int value; } octave_shift;
        struct { int value; } fine_tune;
        struct { const uint8_t* data; int data_len; } exclusive;
    };
} MmfEvent;

// ============ Exclusive voice info (for MA version detection) ============
enum MmfExclusiveType {
    MMF_EXCL_UNKNOWN = 0,
    MMF_EXCL_VM35_VOICE,   // MA-3/MA-5 voice
    MMF_EXCL_VMA_VOICE,    // MA-2 voice
};

typedef struct {
    MmfExclusiveType type;
    int voice_type;
    int bank_msb, bank_lsb, pc, drum_note;
    const uint8_t* data;
    int data_len;
} MmfExclusive;

// ============ Chunk structures ============
typedef struct {
    uint32_t signature;
    uint32_t size;
} MmfChunkHeader;

typedef struct {
    uint8_t contents_class;
    uint8_t contents_type;
    uint8_t contents_code_type;
    uint8_t copy_status;
    uint8_t copy_counts;
    // Options (Shift-JIS decoded)
    char vendor[256];
    char carrier[256];
    char category[256];
    char title[256];
    char artist[256];
    char lyric_writer[256];
    char composer[256];
    char arranger[256];
    char copyright[256];
    // Raw stream for version detection
    const uint8_t* stream;
    int stream_len;
    int has_options;
} MmfContentsInfo;

typedef struct {
    MmfFormatType format_type;
    int sequence_type;
    int duration_time_base_ms;  // Timebase for duration (1/2/4/5/10/20/40/50 ms)
    int gate_time_base_ms;      // Timebase for gate time
    MmfChannelStatus channel_status[16];
} MmfScoreTrack;

typedef struct {
    MmfFormatType format_type;
    std::vector<MmfEvent> events;
    // Channel usage analysis
    int is_channel_used[16];
    int used_channel_count;
    int used_note_count[16];
    uint32_t used_pc[256];     // (bankMSB<<24)|(bankLSB<<16)|(PC<<8)
    int used_pc_count;
    // Program change state per channel (for tracking)
    int channel_bank_msb[16];
    int channel_bank_lsb[16];
    int channel_pc[16];
} MmfSequenceData;

typedef struct {
    MmfExclusive exclusive;
} MmfEXVOChunk;

typedef struct {
    std::vector<MmfExclusive> exclusives;
} MmfSetupData;

// ============ Parsed MMF file ============
typedef struct {
    // Top-level chunks found
    int has_cnti;
    MmfContentsInfo cnti;

    int has_mmmg;
    int track_count;
    MmfScoreTrack* tracks;        // array of track_count
    MmfSequenceData* sequences;   // array of track_count (one per MTR*)
    MmfSetupData* setups;         // array of track_count (one per MTR*, may be empty)

    // All exclusives collected from Mtsu + EXVO
    std::vector<MmfExclusive> all_exclusives;

    // ATR (Audio Track) - ADPCM stream data
    int has_atr;
    int atr_wave_type;          // 0x10=ADPCM 4kHz, 0x11=ADPCM 8kHz
    int atr_timebase_ms;        // timebase in ms
    MmfSequenceData atr_sequence; // Atsq sequence events (same format as Mtsq)
    const uint8_t* atr_wave_data; // ADPCM encoded wave data (points into raw)
    int atr_wave_size;
    int atr_st_tick;             // start tick in Atsq
    int atr_sp_tick;             // stop tick in Atsq

    // MA version detection result
    // 0=unknown, 1=MA-1, 2=MA-2, 3=MA-3, 5=MA-5, 7=MA-7
    int detected_ma_version;
    char detected_ma_name[16];    // "MA-2", "MA-3", etc.

    // Original raw data (not owned, must outlive parser)
    const uint8_t* raw;
    int raw_len;

    // Error info
    char error[512];
} MmfFile;

// ============ Parser API ============

// Optional log callback (set by application to receive parser debug messages)
extern void (*mmf_log_fn)(const char* fmt, ...);

/**
 * Parse an MMF file from raw bytes.
 * @param data Raw MMF file data
 * @param size Size of data
 * @param out Parsed output structure (zeroed and filled)
 * @return 0 on success, -1 on error (check out->error for details)
 */
int mmf_parse(const uint8_t* data, int size, MmfFile* out);

/**
 * Free allocated memory in MmfFile
 */
void mmf_free(MmfFile* f);

/**
 * Detect MA version from CNTI stream field.
 * Looks for "Mx:" pattern in the stream/options text.
 * @param cnti Contents info chunk
 * @return MA version (1-7) or 0 if unknown
 */
int mmf_detect_version_from_cnti(const MmfContentsInfo* cnti);

/**
 * Detect MA version from exclusives (EXVO/Mtsu voice data).
 * Looks for Yamaha SysEx patterns: 0x43 0x79 0x07=MA5, 0x43 0x79 0x06=MA3, 0x43 0x05 0x01=MA5s, 0x43 0x03=MA2
 * @param exclusives Array of exclusive messages
 * @param count Number of exclusives
 * @return MA version (2/3/5) or 0 if unknown
 */
int mmf_detect_version_from_exclusives(const MmfExclusive* exclusives, int count);

/**
 * Shift-JIS to UTF-8 conversion (minimal, for SMAF text fields)
 */
int sjis_to_utf8(const uint8_t* src, int src_len, char* dst, int dst_len);

/**
 * Decode Shift-JIS optional data fields (key-value pairs separated by tag+length)
 * Returns number of fields decoded
 */
int mmf_decode_optional_fields(const uint8_t* stream, int stream_len,
                                char* vendor, int vlen,
                                char* carrier, int clen,
                                char* category, int catlen,
                                char* title, int tlen,
                                char* artist, int alen,
                                char* composer, int swlen);

#ifdef __cplusplus
}
#endif

#endif // MMF_PARSER_H
