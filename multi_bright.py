# multi_bright.py
# pip install pyqt5
import datetime
import os
import re
import sys

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (
    QApplication,
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

CFG_NAME = "brightness.cfg"
LINE = re.compile(r"(\d{2}):(\d{2})-(\d{2}):(\d{2})\s*=\s*(\d{1,3})")

STYLE = """
#Root {
  background-color: #DADADA;
  color: #e8eaed;
  border: 1px solid #2b2f36;
  border-radius: 14px;
}
#Title {
  font-size: 18px;
  font-weight: 700;
  padding: 10px 14px 0px 14px;
}
#Subtitle {
  font-size: 12px;
  color: #b7bcc4;
  padding: 0px 14px 10px 14px;
}
#Status {
  font-size: 12px;
  color: #d5d7db;
  padding: 0px 14px 6px 14px;
}
#CfgInfo {
  font-size: 12px;
  color: #b7bcc4;
  padding: 0px 14px 6px 14px;
}
#Line { color: #2b2f36; margin: 6px 10px; }
#MonName { min-width: 74px; font-weight: 600; }
#Pct { font-size: 13px; font-weight: 800; min-width: 54px; }

QSlider::groove:horizontal {
  height: 6px;
  background: #2b2f36;
  border-radius: 3px;
}
QSlider::sub-page:horizontal {
  background: #8ab4f8;
  border-radius: 3px;
}
QSlider::handle:horizontal {
  width: 18px;
  margin: -6px 0;
  border-radius: 9px;
  background: #8ab4f8;
}
QPushButton {
  background: #DADADA;
  border: 1px solid #2b2f36;
  padding: 10px 12px;
  border-radius: 10px;
  font-weight: 650;
}
QPushButton:hover { background: #DADADA; }
QPushButton:pressed { background: #DADADA; }
"""


def load_schedule(path: str):
    table = []
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for ln in f:
                m = LINE.search(ln)
                if not m:
                    continue
                h1, m1, h2, m2, val = map(int, m.groups())
                val = max(5, min(val, 100))
                table.append(((h1, m1), (h2, m2), val))
    return table


def in_range(now: datetime.time, start, end):
    s = datetime.time(*start)
    e = datetime.time(*end)
    return (s <= now <= e) if (s < e) else (now >= s or now <= e)


def pick_value(table, now: datetime.time):
    for s, e, val in table:
        if in_range(now, s, e):
            return val
    return None


class DimOverlay(QWidget):
    def __init__(self, screen):
        try:
            extra = Qt.WindowTransparentForInput
        except AttributeError:
            extra = 0

        super().__init__(
            None,
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool | extra,
        )
        self.setStyleSheet("background:black;")
        self.setGeometry(screen.geometry())

        self._always_on_top = True
        self._force_blackout = False
        self._last_pct = 100

        self.setWindowOpacity(0.0)
        self.show()

        if extra == 0 and sys.platform == "win32":
            from ctypes import windll

            hwnd = int(self.winId())
            GWL_EXSTYLE = -20
            WS_EX_LAYERED = 0x80000
            WS_EX_TRANSPARENT = 0x20
            style = windll.user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            windll.user32.SetWindowLongW(
                hwnd,
                GWL_EXSTYLE,
                style | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            )

        self.set_always_on_top(True)

    def ensure_topmost(self):
        if self._always_on_top:
            self.set_always_on_top(True)

    def set_always_on_top(self, enabled: bool):
        self._always_on_top = bool(enabled)

        if sys.platform == "win32":
            try:
                from ctypes import windll

                hwnd = int(self.winId())
                HWND_TOPMOST = -1
                HWND_NOTOPMOST = -2
                SWP_NOSIZE = 0x0001
                SWP_NOMOVE = 0x0002
                SWP_NOACTIVATE = 0x0010
                SWP_SHOWWINDOW = 0x0040
                windll.user32.SetWindowPos(
                    hwnd,
                    HWND_TOPMOST if self._always_on_top else HWND_NOTOPMOST,
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW,
                )
            except Exception:
                pass

        try:
            self.setWindowFlag(Qt.WindowStaysOnTopHint, self._always_on_top)
        except Exception:
            pass

        self.show()
        if self._always_on_top:
            self.raise_()

    def set_blackout(self, enabled: bool):
        self._force_blackout = bool(enabled)
        self.set_brightness(self._last_pct)

    def set_brightness(self, pct: int):
        pct = max(5, min(int(pct), 100))
        self._last_pct = pct

        if self._force_blackout:
            self.setWindowOpacity(1.0)
        else:
            self.setWindowOpacity(1.0 - pct / 100.0)


class BrightGUI(QWidget):
    def __init__(self, dimmers, schedule, cfg_path):
        super().__init__(None, Qt.WindowStaysOnTopHint)
        self.setWindowTitle("화면 필터 밝기")
        self.setObjectName("Root")
        self.setStyleSheet(STYLE)
        self.setMinimumWidth(520)

        self.dimmers = dimmers
        self.schedule = schedule
        self.cfg_path = cfg_path

        self.manual_override = False
        self.sliders = []
        self.val_labels = []
        self.blackout_checks = []

        lay = QVBoxLayout(self)
        lay.setContentsMargins(10, 10, 10, 10)
        lay.setSpacing(10)

        title = QLabel("화면 필터 밝기")
        title.setObjectName("Title")
        subtitle = QLabel("슬라이더로 5% 단위 조절, cfg 자동 모드 지원")
        subtitle.setObjectName("Subtitle")

        lay.addWidget(title)
        lay.addWidget(subtitle)

        self.mode_lbl = QLabel("")
        self.mode_lbl.setObjectName("Status")
        self.cfg_lbl = QLabel("")
        self.cfg_lbl.setObjectName("CfgInfo")

        lay.addWidget(self.mode_lbl)
        lay.addWidget(self.cfg_lbl)

        self.chk_topmost = QCheckBox("필터를 항상 최상위로 설정")
        self.chk_topmost.setChecked(True)
        self.chk_topmost.toggled.connect(self.toggle_always_on_top)
        lay.addWidget(self.chk_topmost)

        line = QFrame()
        line.setObjectName("Line")
        line.setFrameShape(QFrame.HLine)
        lay.addWidget(line)

        for i, _d in enumerate(dimmers, 1):
            row = QHBoxLayout()
            name = QLabel(f"모니터 {i}")
            name.setObjectName("MonName")

            s = QSlider(Qt.Horizontal)
            s.setRange(5, 100)
            s.setSingleStep(5)
            s.setPageStep(5)
            s.setTickInterval(5)
            s.setValue(100)

            val_lbl = QLabel("100%")
            val_lbl.setObjectName("Pct")
            val_lbl.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

            chk_blackout = QCheckBox("0%")
            chk_blackout.toggled.connect(self.make_blackout_handler(i - 1))

            s.valueChanged.connect(self.make_handler(i - 1))

            row.addWidget(name, 0)
            row.addWidget(s, 1)
            row.addWidget(val_lbl, 0)
            row.addWidget(chk_blackout, 0)

            lay.addLayout(row)
            self.sliders.append(s)
            self.val_labels.append(val_lbl)
            self.blackout_checks.append(chk_blackout)

        btn_row = QHBoxLayout()
        self.btn_raise = QPushButton("필터 다시 앞으로")
        self.btn_raise.clicked.connect(self.bring_overlays_to_front)

        self.btn_auto = QPushButton("자동(cfg) 모드로")
        self.btn_auto.clicked.connect(self.return_to_auto)

        btn_row.addWidget(self.btn_raise, 1)
        btn_row.addWidget(self.btn_auto, 1)
        lay.addLayout(btn_row)

        self.refresh_status()

        self.timer = QTimer(self)
        self.timer.setInterval(60_000)
        self.timer.timeout.connect(self.tick_auto)
        self.timer.start()

        self.tick_auto(force=True)
        self.toggle_always_on_top(self.chk_topmost.isChecked())

        self.show()
        self.raise_()
        self.activateWindow()

    def refresh_status(self):
        mode = "수동(매뉴얼)" if self.manual_override else "자동(cfg)"
        self.mode_lbl.setText(f"모드: {mode}")

        if self.schedule:
            self.cfg_lbl.setText(f"cfg: {os.path.basename(self.cfg_path)} | 규칙 {len(self.schedule)}개")
        else:
            self.cfg_lbl.setText(f"cfg: {os.path.basename(self.cfg_path)} | 규칙 0개(또는 파일 없음)")

    def make_handler(self, idx: int):
        def _set(v: int):
            self.manual_override = True

            if self.blackout_checks[idx].isChecked():
                self.val_labels[idx].setText("0%")
                self.refresh_status()
                return

            self.val_labels[idx].setText(f"{v}%")
            self.dimmers[idx].set_brightness(v)
            self.refresh_status()

        return _set

    def make_blackout_handler(self, idx: int):
        def _toggle(enabled: bool):
            self.dimmers[idx].set_blackout(enabled)
            self.sliders[idx].setEnabled(not enabled)
            if enabled:
                self.val_labels[idx].setText("0%")
            else:
                v = self.sliders[idx].value()
                self.dimmers[idx].set_brightness(v)
                self.val_labels[idx].setText(f"{v}%")
            self.refresh_status()

        return _toggle

    def apply_slider_values(self):
        for i, d in enumerate(self.dimmers):
            v = self.sliders[i].value()
            d.set_brightness(v)
            if self.blackout_checks[i].isChecked():
                self.val_labels[i].setText("0%")
            else:
                self.val_labels[i].setText(f"{v}%")

    def apply_value_to_all(self, v: int):
        v = max(5, min(int(v), 100))
        for i, d in enumerate(self.dimmers):
            d.set_brightness(v)
            self.sliders[i].blockSignals(True)
            self.sliders[i].setValue(v)
            self.sliders[i].blockSignals(False)
            if self.blackout_checks[i].isChecked():
                self.val_labels[i].setText("0%")
            else:
                self.val_labels[i].setText(f"{v}%")

    def toggle_always_on_top(self, enabled: bool):
        for d in self.dimmers:
            d.set_always_on_top(enabled)

    def tick_auto(self, force: bool = False):
        if self.manual_override and not force:
            return
        if not self.schedule:
            return

        now = datetime.datetime.now().time()
        v = pick_value(self.schedule, now)
        if v is None:
            return

        self.apply_value_to_all(v)

    def return_to_auto(self):
        self.manual_override = False
        self.refresh_status()
        self.tick_auto(force=True)
        self.raise_()
        self.activateWindow()

    def bring_overlays_to_front(self):
        for d in self.dimmers:
            if self.chk_topmost.isChecked():
                d.ensure_topmost()
            d.raise_()
            d.show()

        self.raise_()
        self.activateWindow()


def main():
    base_dir = os.path.dirname(os.path.abspath(sys.argv[0]))
    cfg = os.path.join(base_dir, CFG_NAME)
    sched = load_schedule(cfg)

    app = QApplication(sys.argv)
    dimmers = [DimOverlay(sc) for sc in app.screens()]
    gui = BrightGUI(dimmers, sched, cfg)

    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
