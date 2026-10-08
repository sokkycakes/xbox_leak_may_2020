package xui

import "testing"

func TestLibraryPanel(t *testing.T) {
	res := &Resources{Fonts: map[string]string{}}
	root, err := res.Scene("../../assets/xui/library.xui")
	if err != nil {
		t.Fatal(err)
	}
	skin, err := res.Scene("../../assets/xui/frolick_skin.xui")
	if err != nil {
		t.Fatal(err)
	}
	s := NewScene(root, skin)
	if s.Focus == nil || s.Focus.ID() != "tileSpotlight" {
		t.Fatalf("initial focus %v", s.Focus)
	}
	if !s.Navigate("right") || s.Focus.ID() != "tileExplore" {
		t.Fatal("right")
	}
	if !s.Navigate("down") || s.Focus.ID() != "tileDownloads" {
		t.Fatal("down")
	}
	if s.Navigate("right") {
		t.Fatal("right edge should not move")
	}
	if c := s.ControlAt(219+80, 55+80); c == nil || c.ID() != "tileSpotlight" {
		t.Fatalf("hit %v", c)
	}
	if c := s.ControlAt(10, 10); c != nil {
		t.Fatalf("hit outside %v", c.ID())
	}
	pressed := ""
	s.OnPress = func(n *Node) { pressed = n.ID() }
	s.Press()
	s.Update(1)
	if pressed != "tileDownloads" {
		t.Fatal("press")
	}
}

func TestBladesScenesParse(t *testing.T) {
	pk, err := OpenXZP("/mnt/project-files/xui360/blades-converted/dash_dashuisk.xzp")
	if err != nil {
		t.Skip("Blades packages not present")
	}
	b, _ := pk.Get("skin.xur")
	if _, err := ParseXUR(b); err != nil {
		t.Fatal(err)
	}
}
