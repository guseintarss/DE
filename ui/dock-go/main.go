package main

import (
	"bytes"
	"image/color"
	_ "embed"
	"log"
	"os"
	"strconv"

	"github.com/hajimehoshi/ebiten/v2"
	"github.com/hajimehoshi/ebiten/v2/text/v2"
)

//go:embed assets/SF-Pro-Display-Semibold.otf
var fontData []byte

var tooltipFace *text.GoTextFace

const (
	baseIconSize float32 = 54.0
	maxScale     float32 = 1.85
	sigmaFactor  float32 = 1.05
	iconGap      float32 = 9.0
	trashGap     float32 = 26.0
	padding      float32 = 18.0
	panelRadius  float32 = 26.0
	windowHeight         = 200
	dotReserve   float32 = 13.0
	liftMax      float32 = 11.0
	canvasSize   float32 = 256.0
)

func envInt(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			return n
		}
	}
	return def
}

func textMeasure(s string) (float32, float32) {
	w, h := text.Measure(s, tooltipFace, tooltipFace.Size*1.3)
	return float32(w), float32(h)
}

func textDraw(dst *ebiten.Image, s string, x, y float32, alpha uint8) {
	op := &text.DrawOptions{}
	op.ColorScale.ScaleWithColor(color.NRGBA{255, 255, 255, alpha})
	op.GeoM.Translate(float64(x), float64(y))
	text.Draw(dst, s, tooltipFace, op)
}

func main() {
	src, err := text.NewGoTextFaceSource(bytes.NewReader(fontData))
	if err != nil {
		log.Fatal(err)
	}
	tooltipFace = &text.GoTextFace{Source: src, Size: 15}

	dock := NewDock()
	dock.setupWindow()
	initDBus()
	go watchRunning(dock.apps)

	if err := ebiten.RunGame(dock); err != nil {
		log.Fatal(err)
	}
}
