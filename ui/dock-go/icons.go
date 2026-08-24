package main

import (
	"image/color"
	"math"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/vector"
)

func hexRGB(hex string) (uint8, uint8, uint8) {
	if len(hex) == 7 && hex[0] == '#' {
		return atohex(hex[1:3]), atohex(hex[3:5]), atohex(hex[5:7])
	}
	return 0, 0, 0
}

func atohex(s string) uint8 {
	var v uint8
	for i := 0; i < 2; i++ {
		c := s[i]
		var d uint8
		switch {
		case c >= '0' && c <= '9':
			d = c - '0'
		case c >= 'a' && c <= 'f':
			d = c - 'a' + 10
		case c >= 'A' && c <= 'F':
			d = c - 'A' + 10
		}
		v = v*16 + d
	}
	return v
}

func rgba(hex string, a float64) color.Color {
	r, g, b := hexRGB(hex)
	return color.NRGBA{r, g, b, uint8(a * 255)}
}

func mixRGBA(top, bottom string, t float64, a uint8) color.NRGBA {
	r1, g1, b1 := hexRGB(top)
	r2, g2, b2 := hexRGB(bottom)
	f := func(x, y uint8) uint8 { return uint8(float64(x)*(1-t) + float64(y)*t) }
	return color.NRGBA{f(r1, r2), f(g1, g2), f(b1, b2), a}
}

func colorScale(clr color.Color) (s ebiten.ColorScale) {
	r, g, b, a := clr.RGBA()
	s.Scale(float32(r)/0xffff, float32(g)/0xffff, float32(b)/0xffff, float32(a)/0xffff)
	return s
}

func fillPath(dst *ebiten.Image, p *vector.Path, clr color.Color) {
	vector.FillPath(dst, p, &vector.FillOptions{}, &vector.DrawPathOptions{AntiAlias: true, ColorScale: colorScale(clr)})
}

func strokePath(dst *ebiten.Image, p *vector.Path, w float32, clr color.Color) {
	vector.StrokePath(dst, p, &vector.StrokeOptions{Width: w}, &vector.DrawPathOptions{AntiAlias: true, ColorScale: colorScale(clr)})
}

func roundRect(p *vector.Path, x, y, w, h, r float32) {
	p.MoveTo(x+r, y)
	p.ArcTo(x+w, y, x+w, y+h, r)
	p.ArcTo(x+w, y+h, x, y+h, r)
	p.ArcTo(x, y+h, x, y, r)
	p.ArcTo(x, y, x+w, y, r)
	p.Close()
}

func line(dst *ebiten.Image, x0, y0, x1, y1, w float32, clr color.Color) {
	vector.StrokeLine(dst, x0, y0, x1, y1, w, clr, true)
}

const (
	iconMargin   = 12.0
	iconSide     = canvasSize - 2*iconMargin
	cornerRadius = 58.0
)

func squircleBase(dst *ebiten.Image, top, bottom string) {
	var base vector.Path
	roundRect(&base, iconMargin, iconMargin, iconSide, iconSide, cornerRadius)
	fillPath(dst, &base, mixRGBA(top, bottom, 0.5, 255))

	tmp := ebiten.NewImage(int(canvasSize), int(canvasSize))
	steps := 64
	sh := canvasSize / float32(steps)
	for i := 0; i < steps; i++ {
		t := float64(i) / float64(steps-1)
		col := mixRGBA(top, bottom, t, 255)
		vector.FillRect(tmp, 0, float32(i)*sh, canvasSize, sh+1, col, false)
	}
	op := &ebiten.DrawImageOptions{}
	op.CompositeMode = ebiten.CompositeModeSourceAtop
	dst.DrawImage(tmp, op)

	var gloss vector.Path
	roundRect(&gloss, iconMargin, iconMargin, iconSide, iconSide*0.52, cornerRadius)
	fillPath(dst, &gloss, rgba("#FFFFFF", 0.13))
	var border vector.Path
	roundRect(&border, iconMargin+1.5, iconMargin+1.5, iconSide-3, iconSide-3, cornerRadius-1.5)
	strokePath(dst, &border, 3, rgba("#000000", 0.26))
}

func ellipseStroke(dst *ebiten.Image, cx, cy, rx, ry, w float32, clr color.Color) {
	var p vector.Path
	const n = 72
	for i := 0; i <= n; i++ {
		a := float32(i) / n * 6.28318530718
		x := cx + rx*cos32(a)
		y := cy + ry*sin32(a)
		if i == 0 {
			p.MoveTo(x, y)
		} else {
			p.LineTo(x, y)
		}
	}
	strokePath(dst, &p, w, clr)
}

func cos32(a float32) float32 { return float32(math.Cos(float64(a))) }
func sin32(a float32) float32 { return float32(math.Sin(float64(a))) }

func drawIcon(kind IconKind) *ebiten.Image {
	img := ebiten.NewImage(int(canvasSize), int(canvasSize))
	switch kind {
	case IconFolder:
		squircleBase(img, "#FFD97A", "#F09F1F")
		drawFolder(img)
	case IconTerminal:
		squircleBase(img, "#414957", "#14171E")
		drawTerminal(img)
	case IconGlobe:
		squircleBase(img, "#59C2FF", "#2563EB")
		drawGlobe(img)
	case IconCode:
		squircleBase(img, "#8B7CFF", "#5B3DF5")
		drawCode(img)
	case IconMail:
		squircleBase(img, "#79CDFF", "#2F7BF6")
		drawMail(img)
	case IconMusic:
		squircleBase(img, "#FC7FA4", "#EF3B5D")
		drawMusic(img)
	case IconGear:
		squircleBase(img, "#E6EAF0", "#969EAA")
		drawGear(img)
	case IconTrash:
		squircleBase(img, "#F0F2F5", "#B4BCC9")
		drawTrash(img)
	}
	return img
}

func drawFolder(img *ebiten.Image) {
	var back vector.Path
	roundRect(&back, 62, 84, 132, 100, 16)
	fillPath(img, &back, rgba("#E08A12", 1))
	var tab vector.Path
	roundRect(&tab, 62, 66, 66, 34, 11)
	fillPath(img, &tab, rgba("#E08A12", 1))
	var front vector.Path
	roundRect(&front, 50, 104, 156, 90, 17)
	fillPath(img, &front, rgba("#FFEAB0", 1))
	var sheen vector.Path
	roundRect(&sheen, 50, 104, 156, 22, 17)
	fillPath(img, &sheen, rgba("#FFFFFF", 0.4))
}

func drawTerminal(img *ebiten.Image) {
	var bar vector.Path
	roundRect(&bar, 12, 12, canvasSize-24, 48, 22)
	fillPath(img, &bar, rgba("#FFFFFF", 0.09))
	green := rgba("#4ADE80", 1)
	line(img, 76, 112, 102, 138, 13, green)
	line(img, 102, 138, 76, 164, 13, green)
	vector.FillRect(img, 122, 156, 54, 13, green, true)
}

func drawGlobe(img *ebiten.Image) {
	w := rgba("#FFFFFF", 0.95)
	vector.StrokeCircle(img, 128, 128, 74, 11, w, true)
	line(img, 54, 128, 202, 128, 9, w)
	ellipseStroke(img, 128, 128, 30, 74, 9, w)
	ellipseStroke(img, 128, 128, 57, 74, 7, rgba("#FFFFFF", 0.6))
}

func drawCode(img *ebiten.Image) {
	w := rgba("#FFFFFF", 0.96)
	line(img, 92, 106, 60, 130, 13, w)
	line(img, 60, 130, 92, 154, 13, w)
	line(img, 164, 106, 196, 130, 13, w)
	line(img, 196, 130, 164, 154, 13, w)
	line(img, 142, 94, 114, 168, 12, w)
}

func drawMail(img *ebiten.Image) {
	w := rgba("#FFFFFF", 0.96)
	var env vector.Path
	roundRect(&env, 62, 86, 132, 88, 14)
	strokePath(img, &env, 11, w)
	line(img, 68, 94, 128, 142, 11, w)
	line(img, 128, 142, 188, 94, 11, w)
}

func drawMusic(img *ebiten.Image) {
	w := rgba("#FFFFFF", 0.97)
	vector.FillCircle(img, 96, 178, 23, w, true)
	vector.FillCircle(img, 170, 164, 23, w, true)
	line(img, 117, 176, 117, 82, 13, w)
	line(img, 191, 162, 191, 70, 13, w)
	line(img, 117, 88, 191, 74, 15, w)
}

func drawGear(img *ebiten.Image) {
	silver := rgba("#FBFCFE", 1)
	for k := 0; k < 8; k++ {
		a := float64(k) * math.Pi / 4
		var p vector.Path
		gearTooth(&p, 128, 128, 44, 78, 27, a)
		fillPath(img, &p, silver)
	}
	vector.FillCircle(img, 128, 128, 58, silver, true)
	vector.StrokeCircle(img, 128, 128, 42, 11, rgba("#C3CAD3", 1), false)
	vector.FillCircle(img, 128, 128, 26, rgba("#8A93A0", 1), true)
}

func gearTooth(p *vector.Path, cx, cy, r0, r1, halfW float32, angle float64) {
	s := sin32(float32(angle))
	c := cos32(float32(angle))
	nx, ny := -s, c
	x0, y0 := cx+c*r0, cy+s*r0
	x1, y1 := cx+c*r1, cy+s*r1
	p.MoveTo(x0+nx*halfW*0.55, y0+ny*halfW*0.55)
	p.LineTo(x1+nx*halfW, y1+ny*halfW)
	p.LineTo(x1-nx*halfW, y1-ny*halfW)
	p.LineTo(x0-nx*halfW*0.55, y0-ny*halfW*0.55)
	p.Close()
}

func drawTrash(img *ebiten.Image) {
	lid := rgba("#8E96A3", 1)
	body := rgba("#CBD2DE", 1)
	mesh := rgba("#7E8694", 0.75)
	var handle vector.Path
	roundRect(&handle, 108, 58, 40, 20, 9)
	strokePath(img, &handle, 9, lid)
	var lidP vector.Path
	roundRect(&lidP, 66, 76, 124, 22, 10)
	fillPath(img, &lidP, lid)
	var bodyP vector.Path
	roundRect(&bodyP, 76, 100, 104, 106, 16)
	fillPath(img, &bodyP, body)
	for _, x := range []float32{98, 118, 138, 158} {
		line(img, x, 110, x, 196, 6, mesh)
	}
}
