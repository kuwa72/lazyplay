import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from statistics import mean


def check_links(file, root):
    for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", file.read_text(encoding="utf-8")):
        if "://" in target or target.startswith("#"):
            continue
        target = target.split("#", 1)[0]
        destination = root / target.lstrip("/") if target.startswith("/") else file.parent / target
        assert destination.exists(), f"Broken link in {file}: {target}"


def check_asset(path):
    assert 0 < path.stat().st_size <= 2_500_000, f"Asset too large or empty: {path}"
    metadata = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_entries", "stream=codec_type,width:format=duration",
        "-of", "json", str(path),
    ]))
    assert all(stream["codec_type"] == "video" for stream in metadata["streams"])
    assert metadata["streams"][0]["width"] == 640
    assert 11 <= float(metadata["format"]["duration"]) <= 13


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zenn-root", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    for name in ("README.md", "README.ja.md"):
        check_links(root / name, root)
    asset = root / "assets/ios-mirroring.gif"
    check_asset(asset)
    data = json.loads((root / "assets/windows-mirroring-2026-10-05.json").read_text())
    rows = data["samples"]
    summary = data["summary"]
    assert len(rows) == data["sampleCount"] == 29
    assert all(a[0] < b[0] for a, b in zip(rows, rows[1:]))
    assert rows[-1][0] == data["durationSeconds"]
    average_cpu = 100 * sum(row[1] for row in rows) / data["durationSeconds"] / data["environment"]["logicalProcessors"]
    assert abs(average_cpu - summary["cpuAveragePercent"]) < 0.001
    assert min(row[2] for row in rows) == summary["cpuMinimumPercent"]
    assert max(row[2] for row in rows) == summary["cpuMaximumPercent"]
    for column, name in ((3, "workingSet"), (4, "privateBytes")):
        assert abs(mean(row[column] for row in rows) - summary[f"{name}AverageMiB"]) < 0.01
        assert max(row[column] for row in rows) == summary[f"{name}MaximumMiB"]
    if args.zenn_root:
        article = args.zenn_root / "articles/lazyplay-ios-mac-airplay.md"
        text = article.read_text(encoding="utf-8")
        frontmatter = re.match(r"^---\n(.*?)\n---\n", text, re.S).group(1)
        title = re.search(r'^title: "(.*)"$', frontmatter, re.M).group(1)
        assert len(title) <= 70
        assert re.search(r'^type: "(tech|idea)"$', frontmatter, re.M)
        assert re.search(r"^published: false$", frontmatter, re.M)
        topics = json.loads(re.search(r"^topics: (\[.*\])$", frontmatter, re.M).group(1))
        assert 1 <= len(topics) <= 5 and all(re.fullmatch(r"[a-z0-9-]+", topic) for topic in topics)
        assert re.fullmatch(r"[a-z0-9_-]{12,50}", article.stem)
        check_links(article, args.zenn_root)
        zenn_asset = args.zenn_root / "images/lazyplay-ios-mirroring/demo.gif"
        check_asset(zenn_asset)
        assert hashlib.sha256(asset.read_bytes()).digest() == hashlib.sha256(zenn_asset.read_bytes()).digest()
    print("Adoption docs, measurement arithmetic, demo assets and optional Zenn draft: OK")


if __name__ == "__main__":
    main()
