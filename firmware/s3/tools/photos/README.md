# Plant photos for /capture tests

Put real plant photos (.jpg, .jpeg or .png) here to test plant identification with `tools/fake_nodes.py`:

    python tools/fake_nodes.py <PC IP> --photos tools/photos

The fake CAM's `GET /capture` then cycles through these files in name order, cropped and scaled to the size asked
for (`uxga` 1600×1200 or `sxga` 1280×1024). Without `--photos` it sends the mockup plant scaled up.

The photos are not committed: `.gitignore` leaves out `firmware/s3/tools/photos/*.jpg`, `*.jpeg` and `*.png`.
