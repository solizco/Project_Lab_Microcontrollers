Import("env")
import subprocess
import os
import glob

PROJECT_DIR = env.get("PROJECT_DIR")
CONVERT_CACHE_FILE = os.path.join(PROJECT_DIR, ".convert_cache")
UPLOADFS_CACHE_FILE = os.path.join(PROJECT_DIR, ".uploadfs_cache")

def newest_mtime(paths):
    """Newest modification time across all given files that actually exist."""
    newest = 0
    for p in paths:
        if os.path.exists(p):
            newest = max(newest, os.path.getmtime(p))
    return newest

def read_cached_time(cache_file):
    if os.path.exists(cache_file):
        try:
            with open(cache_file) as f:
                return float(f.read().strip())
        except (ValueError, OSError):
            return 0
    return 0

def write_cached_time(cache_file, timestamp):
    with open(cache_file, "w") as f:
        f.write(str(timestamp))

def run_convert(source, target, env):
    convert_script = os.path.join(PROJECT_DIR, "convert.bat")
    if not os.path.exists(convert_script):
        print("WARNING: convert.bat not found -- skipping clip conversion.")
        return

    gif_files = glob.glob(os.path.join(PROJECT_DIR, "gifs", "*.gif"))
    current_time = newest_mtime(gif_files + [convert_script])
    cached_time = read_cached_time(CONVERT_CACHE_FILE)

    if current_time > 0 and current_time <= cached_time:
        print("=== Source GIFs unchanged since last conversion -- skipping convert.bat ===")
        return

    print("=== Running convert.bat to regenerate clip frames from source GIFs ===")
    result = subprocess.run([convert_script], cwd=PROJECT_DIR, shell=True)
    if result.returncode != 0:
        print("WARNING: convert.bat exited with a non-zero status:", result.returncode)
        return  # don't update the cache on failure -- retry next time

    write_cached_time(CONVERT_CACHE_FILE, current_time)

def data_folder_newest_mtime():
    newest = 0
    data_dir = os.path.join(PROJECT_DIR, "data")
    for root, dirs, files in os.walk(data_dir):
        for f in files:
            newest = max(newest, os.path.getmtime(os.path.join(root, f)))
    return newest

def upload_filesystem_too(source, target, env):
    # The partition table's own mtime is included here on purpose: a
    # partition layout change moves WHERE the filesystem physically
    # lives on the chip, which means the region needs a genuinely fresh
    # write even if not a single clip file changed. Missing this once
    # already produced a real "Corrupted dir pair" mount failure --
    # the firmware expected a filesystem at the new offset, but that
    # region still held stale bytes from the old layout, because the
    # cache (correctly, given only what it was checking) saw no reason
    # to re-upload.
    partitions_file = os.path.join(PROJECT_DIR, "partitions_custom.csv")
    current_time = max(data_folder_newest_mtime(), newest_mtime([partitions_file]))
    cached_time = read_cached_time(UPLOADFS_CACHE_FILE)

    if current_time > 0 and current_time <= cached_time:
        print("=== data/ folder and partition table both unchanged since last filesystem upload -- skipping uploadfs ===")
        return

    print("=== data/ folder or partition table changed -- building + uploading the filesystem image ===")
    result = env.Execute("$PYTHONEXE -m platformio run -t uploadfs -e " + env["PIOENV"])
    if result == 0:
        write_cached_time(UPLOADFS_CACHE_FILE, current_time)
    else:
        print("WARNING: uploadfs failed -- cache not updated, will retry next time")

env.AddPreAction("upload", run_convert)
env.AddPostAction("upload", upload_filesystem_too)