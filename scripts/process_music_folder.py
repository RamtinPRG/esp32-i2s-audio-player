import argparse
import io
import os
import subprocess
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Error: Pillow is required. Install it using: pip install Pillow")
    sys.exit(1)

try:
    import mutagen
except ImportError:
    print("Error: Mutagen is required. Install it using: pip install mutagen")
    sys.exit(1)

# Supported audio extensions
AUDIO_EXTENSIONS = {".mp3", ".flac", ".m4a", ".ogg", ".wav", ".aac", ".wma"}


def extract_cover_bytes(file_path: Path) -> bytes | None:
    """Extracts raw embedded cover art bytes from an audio file."""
    try:
        audio = mutagen.File(file_path)
        if audio is None:
            return None

        # 1. MP3 (ID3 tags - APIC frames)
        if hasattr(audio, "tags") and audio.tags:
            for key in audio.tags.keys():
                if key.startswith("APIC"):
                    return audio.tags[key].data

        # 2. FLAC (FLAC picture blocks)
        if hasattr(audio, "pictures") and audio.pictures:
            return audio.pictures[0].data

        # 3. M4A / MP4 ('covr' atom)
        if hasattr(audio, "get") and audio.get("covr"):
            covr = audio["covr"]
            if covr:
                return bytes(covr[0])

    except Exception as e:
        print(f"  [!] Metadata error on {file_path.name}: {e}")

    return None


def save_bmp_cover(image_bytes: bytes | None, output_bmp_path: Path):
    """Resizes the cover image to 140x140 RGB and saves it as BMP.

    If no image is found, generates a default blank 140x140 BMP.
    """
    try:
        if image_bytes:
            img = Image.open(io.BytesIO(image_bytes))
            img = img.convert("RGB")  # Ensure correct mode for standard BMP
            img = img.resize((140, 140), Image.Resampling.LANCZOS)
            img.save(output_bmp_path, "BMP")
            print(f"  [✓] Cover saved: {output_bmp_path.name}")
            return

    except Exception as e:
        print(
            f"  [!] Failed to parse cover image ({e}). Creating blank fallback.")

    # Fallback: Create a black 140x140 BMP if missing or corrupt
    img = Image.new("RGB", (140, 140), color="black")
    img.save(output_bmp_path, "BMP")
    print(f"  [!] Fallback black BMP created: {output_bmp_path.name}")


def convert_audio_pcm(input_file: Path, output_pcm_path: Path, converter_script: str) -> bool:
    """Executes the external audio_to_pcm.py script."""
    cmd = [sys.executable, converter_script,
           str(input_file), str(output_pcm_path)]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        print(f"  [✓] Audio converted: {output_pcm_path.name}")
        return True
    else:
        print(f"  [✗] Audio conversion failed for {input_file.name}:")
        print(f"      {result.stderr.strip()}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description="Batch process audio files into PCM and 140x140 BMP cover art."
    )
    parser.add_argument("-i", "--input", required=True,
                        help="Input music directory")
    parser.add_argument("-o", "--output", required=True,
                        help="Output directory")
    parser.add_argument(
        "--script",
        default="audio_to_pcm.py",
        help="Path to audio_to_pcm.py script (default: audio_to_pcm.py)",
    )

    args = parser.parse_args()

    input_dir = Path(args.input)
    output_dir = Path(args.output)
    converter_script = args.script

    if not input_dir.exists() or not input_dir.is_dir():
        print(f"Error: Input directory '{input_dir}' does not exist.")
        sys.exit(1)

    if not Path(converter_script).exists():
        print(f"Error: Converter script '{converter_script}' not found.")
        sys.exit(1)

    output_dir.mkdir(parents=True, exist_ok=True)

    # Gather audio files
    audio_files = [
        f for f in input_dir.iterdir() if f.is_file() and f.suffix.lower() in AUDIO_EXTENSIONS
    ]

    if not audio_files:
        print(f"No supported audio files found in '{input_dir}'.")
        return

    print(f"Found {len(audio_files)} audio file(s) to process.\n")

    for idx, audio_path in enumerate(audio_files, start=1):
        base_name = audio_path.stem
        output_pcm = output_dir / f"{base_name}.pcm"
        output_bmp = output_dir / f"{base_name}.bmp"

        print(f"[{idx}/{len(audio_files)}] Processing: {audio_path.name}")

        # 1. Convert Audio to PCM
        convert_audio_pcm(audio_path, output_pcm, converter_script)

        # 2. Extract and Save Cover as 140x140 BMP
        cover_bytes = extract_cover_bytes(audio_path)
        save_bmp_cover(cover_bytes, output_bmp)

        print("-" * 50)

    print(f"\nProcessing complete! Files saved to: {output_dir.resolve()}")


if __name__ == "__main__":
    main()
