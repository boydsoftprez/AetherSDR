package main

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"os"
	"strings"
	"syscall"
	"testing"
	"time"
)

const testToken = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

// Removing the token comparison or accepting a token prefix must leak a report
// here and fail the test. The real collector runs in a harmless local container.
func TestAuthenticationBoundaryAndSingleReport(t *testing.T) {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- serveProbe(ctx, l, testToken) }()
	for _, bad := range []string{strings.Repeat("0", 64) + "\n", testToken + "0\n", testToken[:63] + "\n", testToken + ":forbidden\n", strings.Repeat("0", 64) + ":host-root-fd\n"} {
		c, err := net.Dial("tcp4", l.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		c.SetDeadline(time.Now().Add(4 * time.Second))
		if _, err := io.WriteString(c, bad); err != nil {
			t.Fatal(err)
		}
		got, _ := io.ReadAll(c)
		c.Close()
		if len(got) != 0 {
			t.Fatalf("unauthorized connection received %d bytes", len(got))
		}
	}
	c, err := net.Dial("tcp4", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	c.SetDeadline(time.Now().Add(4 * time.Second))
	if _, err := io.WriteString(c, testToken+"\n"); err != nil {
		t.Fatal(err)
	}
	got, err := io.ReadAll(c)
	c.Close()
	if err != nil {
		t.Fatal(err)
	}
	if len(got) == 0 || len(got) > 16384 {
		t.Fatalf("report length %d", len(got))
	}
	var report map[string]any
	if err := json.Unmarshal(got, &report); err != nil {
		t.Fatal(err)
	}
	if report["goarch"] == "" || report["ipv6_sockets"] == nil || report["paths"] == nil {
		t.Fatal("missing diagnostic fields")
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("server did not stop after one report")
	}
	if c, err := net.DialTimeout("tcp4", l.Addr().String(), time.Second); err == nil {
		c.Close()
		t.Fatal("listener remained open")
	}
}

// Sending no descriptor, a different directory, or a writable file handle
// must fail this real local SCM_RIGHTS boundary test.
func TestAuthenticatedHostRootDescriptor(t *testing.T) {
	if os.Getuid() != 0 {
		t.Skip("requires root receiver for SO_PEERCRED test")
	}
	u, err := net.ListenUnix("unix", &net.UnixAddr{Name: "@nereus-native-rootfd-" + testToken[:16], Net: "unix"})
	if err != nil {
		t.Fatal("local Unix listener unavailable")
	}
	defer u.Close()
	fdResult := make(chan string, 1)
	go func() {
		c, err := u.AcceptUnix()
		if err != nil {
			fdResult <- "Unix accept failed"
			return
		}
		defer c.Close()
		c.SetReadDeadline(time.Now().Add(3 * time.Second))
		payload, oob := make([]byte, 1), make([]byte, syscall.CmsgSpace(4))
		n, oobn, _, _, err := c.ReadMsgUnix(payload, oob)
		if err != nil || n != 1 || payload[0] != 'R' {
			fdResult <- "descriptor message missing"
			return
		}
		messages, err := syscall.ParseSocketControlMessage(oob[:oobn])
		if err != nil || len(messages) != 1 {
			fdResult <- "control message missing"
			return
		}
		fds, err := syscall.ParseUnixRights(&messages[0])
		if err != nil || len(fds) != 1 {
			fdResult <- "one descriptor required"
			return
		}
		f := os.NewFile(uintptr(fds[0]), "received-root")
		defer f.Close()
		got, err := f.Stat()
		if err != nil {
			fdResult <- "descriptor stat failed"
			return
		}
		want, err := os.Stat("/")
		if err != nil || !os.SameFile(got, want) || !got.IsDir() {
			fdResult <- "descriptor did not identify root directory"
			return
		}
		flags, _, errno := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fds[0]), syscall.F_GETFL, 0)
		if errno != 0 || flags&syscall.O_ACCMODE != syscall.O_RDONLY {
			fdResult <- "descriptor was not read-only"
			return
		}
		fdResult <- ""
	}()
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- serveProbe(ctx, l, testToken) }()
	c, err := net.Dial("tcp4", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(4 * time.Second))
	if _, err := io.WriteString(c, testToken+":host-root-fd\n"); err != nil {
		t.Fatal(err)
	}
	b, err := io.ReadAll(c)
	if err != nil {
		t.Fatal(err)
	}
	var status map[string]any
	if err := json.Unmarshal(b, &status); err != nil {
		t.Fatal("bridge operation did not return JSON status")
	}
	if status["bridge_sent"] != true || status["peer_uid"] != float64(0) {
		t.Fatal("bridge did not report root peer delivery")
	}
	select {
	case failure := <-fdResult:
		if failure != "" {
			t.Fatal(failure)
		}
	case <-time.After(time.Second):
		t.Fatal("descriptor not received")
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("bridge server did not exit")
	}
}

// Ignoring the overall context while waiting for an auth line fails this test.
func TestDeadlineClosesIdleConnection(t *testing.T) {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- serveProbe(ctx, l, testToken) }()
	c, err := net.Dial("tcp4", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(time.Second))
	got, _ := io.ReadAll(c)
	if len(got) != 0 {
		t.Fatal("idle connection received report")
	}
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("deadline failed to terminate server")
	}
}

// JSON escaping can multiply source text size. Removing the encoded-length
// check fails this independent worst-case fixture, even with bounded reads.
func TestReportEncodedSizeBound(t *testing.T) {
	r := collectReport()
	for k := range r.Text {
		r.Text[k] = strings.Repeat("\x00", 4096)
	}
	got, err := encodeReport(r)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) > 16384 {
		t.Fatalf("encoded report grew to %d bytes", len(got))
	}
	if !json.Valid(got) {
		t.Fatal("size limiting damaged JSON")
	}
}

// Missing ownership/access queries, supplementary groups, or the explicit
// read-only administrative command must fail this real collector boundary.
func TestAdministrativeMetadata(t *testing.T) {
	b, err := encodeReport(collectReport())
	if err != nil {
		t.Fatal(err)
	}
	var r map[string]any
	if err := json.Unmarshal(b, &r); err != nil {
		t.Fatal(err)
	}
	paths := r["paths"].(map[string]any)
	if paths["/boot/cmdline.txt"] == nil {
		t.Fatal("boot command-line access metadata missing")
	}
	root := paths["/proc/1/root"].(map[string]any)
	if root["uid"] == nil || root["gid"] == nil || root["writable"] == nil || root["access_errno"] == nil {
		t.Fatal("ownership or access check missing")
	}
	status := r["process_status"].(map[string]any)
	if status["Groups"] == nil {
		t.Fatal("supplementary groups missing")
	}
	text := r["text"].(map[string]any)
	if out, ok := text["command_id"].(string); !ok || !strings.Contains(out, "uid=") {
		t.Fatal("read-only id command missing")
	}
	if r["privileged_executables"] == nil || text["process_executable"] == nil || text["process_cwd"] == nil {
		t.Fatal("administrative diagnostic metadata missing")
	}
}
