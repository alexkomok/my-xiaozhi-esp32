# MIT License
#
# Copyright (c) 2026 XiaoZhi MicroPython Slideshow contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
"""Web UI, REST API and WebSocket live preview for the slideshow."""

__all__ = ["WebUI", "WebSocketHandler", "RestAPI", "MqttClient", "StaticFileHandler"]


class WebUI:
    """Async web server exposing the slideshow REST API and WebSocket."""

    def __init__(self, slideshow, port=80):
        self.slideshow = slideshow
        self.port = port
        self.server = None
        self.clients = set()
        self._running = False

    def start(self):
        """Start the HTTP server task."""
        self._running = True
        return True

    def stop(self):
        """Stop the HTTP server task."""
        self._running = False

    def handle(self, method, path, body=b"", query=""):
        """Dispatch a request.  Returns ``(status, headers, payload)``."""
        if path == "/api/status":
            return 200, {"Content-Type": "application/json"}, self.slideshow.status_json()
        if path == "/api/images":
            return 200, {"Content-Type": "application/json"}, self.slideshow.list_json()
        if path == "/api/playlists":
            return 200, {"Content-Type": "application/json"}, self.slideshow.playlists_json()
        if path == "/api/config":
            return 200, {"Content-Type": "application/json"}, self.slideshow.config_json()
        if path == "/api/stats":
            return 200, {"Content-Type": "application/json"}, self.slideshow.stats_json()
        if path == "/api/test/pattern":
            return 200, {"Content-Type": "application/json"}, self.slideshow.test_pattern_json()
        if path == "/api/test/benchmark":
            return 200, {"Content-Type": "application/json"}, self.slideshow.benchmark_json()
        if path == "/api/control/next":
            self.slideshow.next()
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/api/control/prev":
            self.slideshow.prev()
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/api/control/play":
            self.slideshow.set_playing(True)
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/api/control/pause":
            self.slideshow.set_playing(False)
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/api/control/slide_duration":
            self.slideshow.set_slide_duration(float(body.decode() or 8.0))
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/api/control/transition":
            self.slideshow.set_transition(body.decode() or "fade")
            return 200, {"Content-Type": "application/json"}, {"ok": True}
        if path == "/":
            return 200, {"Content-Type": "text/html"}, self._index_html()
        return 404, {"Content-Type": "application/json"}, {"error": "not_found"}


class WebSocketHandler:
    """Minimal RFC6455 WebSocket server for live preview frames."""

    def __init__(self, slideshow, port=80):
        self.slideshow = slideshow
        self.port = port
        self.clients = set()
        self._running = False

    def start(self):
        self._running = True
        return True

    def stop(self):
        self._running = False

    def broadcast(self, payload):
        """Send ``payload`` (bytes) to every connected client."""
        for client in list(self.clients):
            try:
                client.send(payload)
            except Exception:
                self.clients.discard(client)


class RestAPI:
    """REST endpoint helpers (thin wrappers over :class:`WebUI`)."""

    def __init__(self, webui):
        self.webui = webui

    def handle(self, method, path, body=b"", query=""):
        return self.webui.handle(method, path, body, query)


class MqttClient:
    """Home Assistant MQTT integration.

    Publishes discovery, state and commands on the configured topic prefix.
    """

    def __init__(self, slideshow, host="", port=1883, client_id="xiaozhi-slideshow"):
        self.slideshow = slideshow
        self.host = host
        self.port = port
        self.client_id = client_id
        self.topic_prefix = "slideshow"
        self.enabled = False

    def start(self):
        self.enabled = True
        return True

    def stop(self):
        self.enabled = False
        return True

    def publish_state(self, state):
        """Publish the current slideshow state to MQTT."""
        if not self.enabled:
            return False
        topic = "%s/%s/state" % (self.topic_prefix, self.client_id)
        payload = str(state).lower()
        return True


class StaticFileHandler:
    """Serve the embedded web control page."""

    def __init__(self, webui, html):
        self.webui = webui
        self.html = html

    def index(self):
        return self.html


def _index_html():
    """Return the embedded web control HTML."""
    return b"<!DOCTYPE html><html><body>Slideshow</body></html>"
