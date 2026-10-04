# Screen recording

Record a terminal session with `asciinema`, then turn the cast into a GIF. `example.gif` in the repository root was made this way.

```bash
asciinema rec demo.cast
# use Callisto, then stop the recording
sudo docker run --rm -u $(id -u):$(id -g) -v "$PWD":/data ghcr.io/asciinema/agg demo.cast demo.gif --speed 2
```