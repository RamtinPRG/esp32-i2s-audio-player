import argparse
import sys
import numpy as np


def main():
    parser = argparse.ArgumentParser(
        description="Convert audio file to C header for I2S (44.1kHz, 16-bit, Stereo)")
    parser.add_argument(
        "input_file", help="Path to the input audio file (e.g., mp3, wav, flac)")
    parser.add_argument(
        "output_header", help="Path to the output C header file (e.g., audio_data.h)")
    parser.add_argument("-n", "--name", default="audio_data",
                        help="Name of the C array (default: audio_data)")

    args = parser.parse_args()

    try:
        from pydub import AudioSegment
    except ImportError:
        print("Error: The 'pydub' library is required. Install it using: pip install pydub numpy")
        print("Note: You also need 'ffmpeg' installed on your system and available in your PATH.")
        sys.exit(1)

    try:
        print(f"Loading audio file: {args.input_file}")
        audio = AudioSegment.from_file(args.input_file)
    except Exception as e:
        print(f"Error loading audio file: {e}")
        print("Make sure you have 'ffmpeg' installed and the file format is supported.")
        sys.exit(1)

    print(
        f"Original format: {audio.frame_rate}Hz, {audio.sample_width*8}bit, {audio.channels} channels")

    # Convert to target specifications
    # 44100 Hz, 16-bit (2 bytes), Stereo (2 channels)
    print("Converting to 44.1kHz, 16-bit, Stereo...")
    target_audio = audio.set_frame_rate(
        44100).set_sample_width(2).set_channels(2)

    print(
        f"Target format: {target_audio.frame_rate}Hz, {target_audio.sample_width*8}bit, {target_audio.channels} channels")
    print(f"Duration: {len(target_audio) / 1000.0:.2f} seconds")

    # Get raw bytes
    raw_bytes = target_audio.raw_data

    # Convert to numpy array of int16
    # By writing decimal integers to the C file, we bypass endianness differences
    # between the host PC and the ESP32. The C compiler will handle endianness correctly.
    audio_array = np.frombuffer(raw_bytes, dtype=np.int16)

    total_samples = len(audio_array)
    total_bytes = len(raw_bytes)

    print(f"Total frames (stereo pairs): {total_samples // 2}")
    print(f"Total samples (L+R): {total_samples}")
    print(f"Total bytes: {total_bytes}")

    if total_bytes > 1000000:
        print("\n[WARNING] The generated array is larger than 1MB. This might exceed your ESP32's available RAM/Flash")
        print("or cause compiler memory issues. Consider storing large files in SPIFFS/LittleFS or an SD card instead.\n")

    # Write to C header
    print(f"Writing C header to: {args.output_header}")
    with open(args.output_header, "w") as f:
        header_guard = args.name.upper() + "_H"
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <stddef.h>\n\n")

        # Write metadata
        f.write(f"// Audio properties\n")
        f.write(f"// Sample Rate: 44100 Hz\n")
        f.write(f"// Bit Depth: 16-bit\n")
        f.write(f"// Channels: Stereo (2)\n")
        f.write(f"// Duration: {len(target_audio) / 1000.0:.2f} seconds\n\n")

        # Write lengths
        f.write(f"const size_t {args.name}_len_bytes = {total_bytes};\n")
        f.write(
            f"const size_t {args.name}_len_samples = {total_samples}; // Total int16_t elements\n\n")

        f.write(f"const int16_t {args.name}[] = {{\n")

        # Write data in chunks to avoid memory issues and make it readable
        # 16 samples per line = 32 bytes per line
        chunk_size = 16
        for i in range(0, total_samples, chunk_size):
            chunk = audio_array[i:i+chunk_size]
            line = ", ".join(f"{x:6d}" for x in chunk)
            f.write(f"    {line},\n")

        f.write("};\n\n")
        f.write("#endif\n")

    print("Conversion complete!")


if __name__ == "__main__":
    main()
