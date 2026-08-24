package main

import (
	"image/color"
	"math"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/inpututil"
	"github.com/hajimehoshi/ebiten/v2/vector"
)

type Dock struct {
	apps  []App
	icons []*ebiten.Image

	scales  []float32
	lift    []float32
	bounceT []float32

	winW int

	panelX0, panelX1, panelTop, panelBot float32

	hover        int
	tooltipAlpha float32
	passthrough  bool
}

func NewDock() *Dock {
	d := &Dock{apps: defaultApps(), hover: -1}
	for _, a := range d.apps {
		d.icons = append(d.icons, drawIcon(a.Icon))
		d.scales = append(d.scales, 1)
		d.lift = append(d.lift, 0)
		d.bounceT = append(d.bounceT, 0)
	}
	return d
}

func (d *Dock) magnifiedWidth() int {
	n := len(d.apps)
	w := 0.0
	for range n {
		w += float64(baseIconSize * maxScale)
	}
	gaps := float64(float32(n-2)*iconGap + trashGap)
	return int(math.Ceil(w + gaps + float64(padding*2) + 48))
}

func (d *Dock) setupWindow() {
	sw := envInt("DOCK_SCREEN_W", 0)
	sh := envInt("DOCK_SCREEN_H", 0)
	if sw <= 0 || sh <= 0 {
		fw, fh := ebiten.ScreenSizeInFullscreen()
		if sw <= 0 {
			sw = fw
		}
		if sh <= 0 {
			sh = fh
		}
	}
	if sw <= 0 {
		sw = 1280
	}
	if sh <= 0 {
		sh = 720
	}

	d.winW = min(d.magnifiedWidth(), sw-24)

	ebiten.SetWindowSize(d.winW, windowHeight)
	ebiten.SetWindowDecorated(false)
	ebiten.SetWindowFloating(true)
	ebiten.SetScreenTransparent(true)
	ebiten.SetWindowResizable(false)
	ebiten.SetWindowPosition((sw-d.winW)/2, sh-windowHeight)
	ebiten.SetWindowTitle("Dock")
}

func (d *Dock) layout() (centers, widths []float32) {
	n := len(d.apps)
	widths = make([]float32, n)
	total := padding * 2
	for i := range n {
		widths[i] = baseIconSize * d.scales[i]
		total += widths[i]
		if i > 0 {
			gap := iconGap
			if i == n-1 {
				gap = trashGap
			}
			total += gap
		}
	}
	x := (float32(d.winW) - total) / 2
	centers = make([]float32, n)
	for i := range n {
		centers[i] = x + widths[i]/2
		x += widths[i]
		if i < n-1 {
			if i == n-2 {
				x += trashGap
			} else {
				x += iconGap
			}
		}
	}
	d.panelX0 = (float32(d.winW) - total) / 2
	d.panelX1 = d.panelX0 + total
	ph := baseIconSize*maxScale + padding*2 + dotReserve
	d.panelTop = windowHeight - ph - 4
	d.panelBot = d.panelTop + ph
	return centers, widths
}

func (d *Dock) iconBottom() float32 {
	return d.panelBot - padding - dotReserve
}

func (d *Dock) Update() error {
	tps := ebiten.ActualTPS()
	if tps <= 1 {
		tps = 60
	}
	dt := 1 / float32(tps)

	mx, my := ebiten.CursorPosition()
	fx, fy := float32(mx), float32(my)
	w, h := d.LayoutSize()
	inside := fx >= 0 && fx < float32(w) && fy >= 0 && fy < float32(h)

	centers, widths := d.layout()

	inBand := fy > d.panelTop-60 && fy < d.panelBot
	hover := -1
	if inside && inBand {
		for i := range centers {
			if math.Abs(float64(fx-centers[i])) <= float64(widths[i])/2+9 {
				hover = i
			}
		}
	}
	d.hover = hover

	sigma := baseIconSize * sigmaFactor
	var2 := 2 * sigma * sigma
	for i := range d.apps {
		target := float32(1)
		f := float32(0)
		if hover >= 0 {
			dx := fx - centers[i]
			f = float32(math.Exp(-float64(dx*dx) / float64(var2)))
			target = 1 + (maxScale-1)*f
		}
		k := min(1, dt*13)
		d.scales[i] += (target - d.scales[i]) * k
		d.lift[i] += (liftMax*f - d.lift[i]) * min(1, dt*12)

		t := d.bounceT[i]
		if t > 0 {
			t += dt
			if t > 1.5 {
				t = 0
			}
			d.bounceT[i] = t
		}
	}

	if hover >= 0 && inpututil.IsMouseButtonJustPressed(ebiten.MouseButtonLeft) {
		d.bounceT[hover] = 0.0001
		app := d.apps[hover]
		go launch(app)
	}

	over := inside && fx >= d.panelX0 && fx <= d.panelX1 && fy >= d.panelTop && fy <= d.panelBot
	desired := !over
	if d.passthrough != desired {
		ebiten.SetWindowMousePassthrough(desired)
		d.passthrough = desired
	}

	ta := float32(0)
	if hover >= 0 {
		ta = 1
	}
	d.tooltipAlpha += (ta - d.tooltipAlpha) * min(1, dt*11)

	return nil
}

func (d *Dock) Draw(screen *ebiten.Image) {
	screen.Fill(color.NRGBA{0, 0, 0, 0})

	centers, widths := d.layout()
	n := len(d.apps)

	for i, grow := range []float32{7, 15, 26} {
		var p vector.Path
		a := []float32{0.05, 0.035, 0.02}[i]
		roundRect(&p, d.panelX0-grow, d.panelTop-grow, d.panelX1-d.panelX0+grow*2, d.panelBot-d.panelTop+grow*2, panelRadius+grow)
		fillPath(screen, &p, color.NRGBA{0, 0, 0, uint8(a * 255)})
	}

	var panel vector.Path
	roundRect(&panel, d.panelX0, d.panelTop, d.panelX1-d.panelX0, d.panelBot-d.panelTop, panelRadius)
	fillPath(screen, &panel, color.NRGBA{22, 22, 27, 186})
	var inner vector.Path
	roundRect(&inner, d.panelX0+1, d.panelTop+1, d.panelX1-d.panelX0-2, d.panelBot-d.panelTop-2, panelRadius-1)
	strokePath(screen, &inner, 2, color.NRGBA{255, 255, 255, 34})

	if n > 1 {
		sx := (centers[n-2] + widths[n-2]/2 + centers[n-1] - widths[n-1]/2) / 2
		line(screen, sx, d.panelTop+18, sx, d.panelBot-18, 2, color.NRGBA{255, 255, 255, 42})
	}

	bottomY := d.iconBottom()
	for i := range d.apps {
		s := d.scales[i]
		size := baseIconSize * s
		boff := bounceOffset(d.bounceT[i])
		y := bottomY - size - d.lift[i] - boff
		op := &ebiten.DrawImageOptions{}
		op.Filter = ebiten.FilterLinear
		op.GeoM.Scale(float64(size/canvasSize), float64(size/canvasSize))
		op.GeoM.Translate(float64(centers[i]-size/2), float64(y))
		screen.DrawImage(d.icons[i], op)
	}

	for i := range d.apps {
		if isRunning(d.apps[i].Name) {
			vector.FillCircle(screen, centers[i], d.panelBot-10, 2.6, color.NRGBA{255, 255, 255, 235}, true)
		}
	}

	if d.tooltipAlpha > 0.03 && d.hover >= 0 {
		i := d.hover
		name := d.apps[i].Name
		tw, th := textMeasure(name)
		pw := tw + 28
		phh := float32(31)
		px := centers[i] - pw/2
		px = max(4, min(px, float32(d.winW)-pw-4))
		py := bottomY - baseIconSize*d.scales[i] - d.lift[i] - phh - 12
		var p vector.Path
		roundRect(&p, px, py, pw, phh, 10)
		fillPath(screen, &p, color.NRGBA{28, 28, 32, uint8(215 * d.tooltipAlpha)})
		textDraw(screen, name, px+(pw-tw)/2, py+(phh-th)/2-1, uint8(255*d.tooltipAlpha))
	}
}

func bounceOffset(t float32) float32 {
	if t <= 0 {
		return 0
	}
	return 30 * float32(math.Sin(float64(t)*11)) * float32(math.Exp(-float64(t)*3.2))
}

func (d *Dock) Layout(outWidth, outHeight int) (int, int) {
	return d.winW, windowHeight
}

func (d *Dock) LayoutSize() (int, int) {
	return d.winW, windowHeight
}
