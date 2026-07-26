# Headless binding test (SDL_VIDEODRIVER=dummy):
#   micropython bindings/surfer/test_surfer.py
# Exercises the M5 acceptance shapes: surfer.slider(x,y), screen.add(s),
# s.y_pos, s.callback = fn — plus grids, labels, and a touch-driven cb.
import surfer

fails = []


def ok(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL:", msg)


surfer.init(640, 400)
screen_node = surfer.screen()

# node basics
g = surfer.group(10, 20)
screen_node.add(g)
ok(g.x_pos == 10 and g.y_pos == 20, "group pos")
g.x_pos = 30
ok(g.x_pos == 30, "x_pos setter")

r = surfer.rect(0, 0, 50, 40, surfer.rgb(255, 0, 0))
g.add(r)
ok(r.w == 50 and r.h == 40, "rect size")

lbl = surfer.label("hello surfer", 5, 60)
g.add(lbl)
ok(lbl.h > 0, "label measured")
lbl.set_text("changed")

# widget: the user-facing shape from the milestone
s = surfer.slider(200, 50)
screen_node.add(s)
ok(s.y_pos == 50, "slider y_pos")
s.y_pos = 60
ok(s.y_pos == 60, "slider y_pos setter")
s.value = 0.5
ok(abs(s.value - 0.5) < 0.01, "slider value roundtrip")

hits = []
s.callback = lambda v: hits.append(v)

# drag the slider cap via injected touches (real dispatch path)
surfer.tick()
surfer._touch(224, 220, surfer.TOUCH_DOWN)
surfer._touch(224, 120, surfer.TOUCH_MOVE)
surfer._touch(224, 120, surfer.TOUCH_UP)
surfer.tick()
ok(len(hits) > 0, "slider callback fired from touch")
ok(hits[-1] > 0.5, "drag up raised the value")

k = surfer.knob(400, 60)
screen_node.add(k)
k.value = 0.25
ok(abs(k.value - 0.25) < 0.02, "knob value")

c = surfer.checkbox(500, 60)
screen_node.add(c)
ok(c.value is False, "checkbox unchecked")
surfer._touch(510, 70, surfer.TOUCH_DOWN)
surfer._touch(510, 70, surfer.TOUCH_UP)
surfer.tick()
ok(c.value is True, "checkbox toggled by tap")

d = surfer.dropdown(200, 300, 140, ["sine", "saw", "square"])
screen_node.add(d)
picks = []
d.callback = lambda i: picks.append(i)
ok(d.value == 0, "dropdown initial")
d.value = 2
ok(d.value == 2 and not picks, "programmatic select fires no cb")

# textgrid
tg = surfer.textgrid(20, 4)
screen_node.add(tg)
tg.set_row(0, "hello grid")
tg.grid_scroll(1)
tg.set_cell(0, 3, "X")

# textinput: one line of editable text. It draws the TEXT only — the box
# is the caller's rect — and it takes keys one event at a time from
# surfer.keys(), so an app never has to retype the edit dispatch.
ti = surfer.textinput(20, 340, 300)
screen_node.add(ti)
ti.focus()
ti.text = "hello"
ok(ti.text == "hello" and ti.caret == 5, "textinput text + caret")
ok(ti.key((surfer.KEY_TEXT, "!", False)) is True, "key() consumed text")
ok(ti.text == "hello!", "typed at the caret")
ti.key((surfer.KEY_BACKSPACE, "", False))
ti.key((surfer.KEY_HOME, "", False))
ok(ti.text == "hello" and ti.caret == 0, "backspace + home")
ok(ti.key((surfer.KEY_ENTER, "", False)) is False, "enter is the app's")
ti.insert("say ")
ok(ti.text == "say hello", "insert at caret")

# a tap places the caret where the finger landed — the binding wires
# that at creation, since it is what a text field IS
surfer.tick()
want = ti.index_from_x(24)
surfer._touch(20 + 24, 346, surfer.TOUCH_DOWN)
surfer._touch(20 + 24, 346, surfer.TOUCH_UP)
surfer.tick()
ok(ti.caret == want and want > 0, "tap placed the caret")

# ...and a Python on_touch still fires, after the caret has moved
taps = []
ti.on_touch = lambda ph, x, y: taps.append(ph)
surfer._touch(20 + 60, 346, surfer.TOUCH_DOWN)
surfer._touch(20 + 60, 346, surfer.TOUCH_UP)
surfer.tick()
ok(len(taps) == 2 and ti.caret != want, "on_touch survives the caret wiring")

# the same methods on a node that is not a field do nothing at all
lbl = surfer.label("plain", 0, 0)
lbl.insert("x")
lbl.key((surfer.KEY_TEXT, "x", False))
ok(True, "textinput methods are safe on other nodes")

# detach keeps state (the multitasking primitive)
g.detach()
screen_node.add(g)

for _ in range(5):
    surfer.tick()

print("FAILURES:" if fails else "ALL OK,", len(fails) if fails else "surfer mpy binding good")
