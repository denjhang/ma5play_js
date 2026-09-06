CXX = g++
CXXFLAGS = -O2 -std=c++17 -fwrapv -Isrc -Iref/midifile/include

SRCS = src/ymf_data.cpp src/envelope_gen.cpp src/voice_lib.cpp src/midi_sequencer.cpp src/mmf_parser.cpp src/smaf_sequencer.cpp src/adpcm_decoder.cpp src/wave_drum.cpp src/ma3_engine.cpp src/main.cpp
REF_SRCS = ref/midifile/src/Binasc.cpp ref/midifile/src/MidiEvent.cpp ref/midifile/src/MidiEventList.cpp \
           ref/midifile/src/MidiFile.cpp ref/midifile/src/MidiMessage.cpp

TARGET = ymf825emu.exe

all: $(TARGET)

$(TARGET): $(SRCS) $(REF_SRCS) src/*.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(REF_SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
