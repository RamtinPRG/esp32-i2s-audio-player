import argparse
import sys
from pydub import AudioSegment

TARGET_PEAK_DBFS = -25.0  # Try -3.0, -6.0, or -9.0 if still harsh


def main():
    parser = argparse.ArgumentParser(
        description="Convert audio to raw PCM with safe peak level")
    parser.add_argument("input_file", help="Input audio file")
    parser.add_argument(
        "output_file", help="Output raw PCM file, e.g. main/spiffs_image/audio.pcm")
    args = parser.parse_args()

    try:
        audio = AudioSegment.from_file(args.input_file)

        # Convert channel layout first
        audio = audio.set_channels(2)

        # Reduce peak level if it is too high
        if audio.max_dBFS != float("-inf") and audio.max_dBFS > TARGET_PEAK_DBFS:
            gain_change = TARGET_PEAK_DBFS - audio.max_dBFS
            print(f"Reducing gain by {gain_change:.2f} dB to avoid clipping")
            audio = audio.apply_gain(gain_change)

        # Now convert to target format
        target_audio = audio.set_frame_rate(44100).set_sample_width(2)

        raw_bytes = target_audio.raw_data

        with open(args.output_file, "wb") as f:
            f.write(raw_bytes)

        print(f"Success! Saved {len(raw_bytes)} bytes to {args.output_file}")

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
