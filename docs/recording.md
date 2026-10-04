# Screen Recording

You can screen-record into a GIF using `asciinema`.

```bash
asciinema rec demo.cast
# Do stuff
# Then convert to GIF
sudo docker run --rm -u $(id -u):$(id -g) -v $PWD:/data ghcr.io/asciinema/agg demo.cast demo.gif --speed 2
```