"""Close-up screens whose painted text the player has to read.

Shared by tools/upscale_transformer.py (--apply-text-screens: JPEG deblock + 4x) and
tools/extract_page_text.py (OCR of the page text for the in-game page reader).
Entries are mha2.dat image stems; the HD files are hd_assets/<stem>.webp and the
reader text hd_assets/<stem>.txt.
"""

TEXT_SCREENS = (
    # open books / diaries held by the player
    "m/005/img/objBg", "m/006/img/objBg", "m/017/img/objBg",
    "m/018/img/objBg1", "m/018/img/objBg2", "m/019/img/objBg1", "m/019/img/objBg2",
    "m/021/img/objBg", "m/022/img/objBg",
    # loose pages and parchments
    "m/020/img/objBg1", "m/020/img/objBg2", "m/023/img/objBg",
    "m/013/img/objBg",   # tombstone inscription
    "m/029/img/objBg",   # notice board
    "s/008/img/objBg",   # treasure map
    "l/044/img/objBg",   # flight map with travel diary paragraph
)

# Upscaled with the text pipeline but without a page reader: their lettering is already
# large (map labels, flight briefing) and the briefing leads straight into the flight
# minigame, where the hotspot key must not be taken over.
NO_READER = ("s/008/img/objBg", "l/044/img/objBg")

# Text area of every page in 800x450 game coordinates, reading order (left page first).
# Two-page spreads are split exactly at the gutter: OCR models read across a spread
# line by line otherwise.
PAGE_BOXES = {
    "m/005/img/objBg":  [(140, 40, 372, 360), (372, 40, 620, 360)],
    "m/006/img/objBg":  [(150, 30, 404, 370), (404, 30, 650, 370)],
    "m/017/img/objBg":  [(140, 40, 400, 375), (400, 40, 650, 375)],
    "m/018/img/objBg1": [(150, 40, 400, 390), (400, 40, 650, 390)],
    "m/018/img/objBg2": [(150, 40, 400, 390), (400, 40, 650, 390)],
    "m/019/img/objBg1": [(195, 160, 400, 420), (400, 160, 600, 420)],
    "m/019/img/objBg2": [(195, 160, 400, 420), (400, 160, 600, 420)],
    "m/020/img/objBg1": [(230, 10, 570, 440)],
    "m/020/img/objBg2": [(230, 10, 570, 440)],
    "m/021/img/objBg":  [(140, 50, 400, 370), (400, 50, 650, 370)],
    "m/022/img/objBg":  [(180, 50, 408, 370), (408, 50, 640, 370)],
    "m/023/img/objBg":  [(150, 70, 640, 370)],
    "m/013/img/objBg":  [(250, 300, 580, 410)],
    "m/029/img/objBg":  [(0, 0, 800, 450)],
}
