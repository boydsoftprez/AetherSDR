// SPDX-License-Identifier: MIT
// Copyright (c) 2026 J. J. Boyd
// Original diagnostic code. AI tooling: OpenAI Codex, 2026-10-09.
package main

import (
	"bufio"
	"context"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"time"
)

// Populated at build time. Never written to stdout, stderr or the report.
var authTokenHex string

type pathMetadata struct {
	Exists          bool    `json:"exists"`
	Mode            string  `json:"mode,omitempty"`
	Size            int64   `json:"size,omitempty"`
	Symlink         string  `json:"symlink,omitempty"`
	Error           string  `json:"error,omitempty"`
	UID             *uint32 `json:"uid,omitempty"`
	GID             *uint32 `json:"gid,omitempty"`
	Writable        bool    `json:"writable"`
	AccessErrno     int     `json:"access_errno"`
	SetUID          bool    `json:"setuid,omitempty"`
	SetGID          bool    `json:"setgid,omitempty"`
	FileCapability  string  `json:"file_capability_hex,omitempty"`
	CapabilityError string  `json:"file_capability_error,omitempty"`
}
type socketResult struct {
	Created bool   `json:"created"`
	Errno   int    `json:"errno"`
	Error   string `json:"error,omitempty"`
}
type report struct {
	GOARCH                  string                  `json:"goarch"`
	Text                    map[string]string       `json:"text"`
	Status                  map[string]string       `json:"process_status"`
	Namespaces              map[string]string       `json:"namespaces"`
	Paths                   map[string]pathMetadata `json:"paths"`
	BootEntries             []string                `json:"boot_entries"`
	BootError               string                  `json:"boot_error,omitempty"`
	IPv6Sockets             map[string]socketResult `json:"ipv6_sockets"`
	Truncated               bool                    `json:"truncated"`
	PrivilegedExecutables   map[string]pathMetadata `json:"privileged_executables"`
	ExecutableScanTruncated bool                    `json:"executable_scan_truncated"`
}

func inspectPath(path string, capability bool) pathMetadata {
	m := pathMetadata{}
	err := syscall.Access(path, 2) // POSIX W_OK: permission query only, no write.
	m.Writable = err == nil
	var errno syscall.Errno
	if errors.As(err, &errno) {
		m.AccessErrno = int(errno)
	}
	info, err := os.Lstat(path)
	if err != nil {
		m.Error = short(err.Error(), 96)
		return m
	}
	m.Exists, m.Mode, m.Size = true, info.Mode().String(), info.Size()
	m.SetUID, m.SetGID = info.Mode()&os.ModeSetuid != 0, info.Mode()&os.ModeSetgid != 0
	if st, ok := info.Sys().(*syscall.Stat_t); ok {
		uid, gid := st.Uid, st.Gid
		m.UID, m.GID = &uid, &gid
	}
	if info.Mode()&os.ModeSymlink != 0 {
		target, _ := os.Readlink(path)
		m.Symlink = short(target, 512)
	}
	if capability {
		buf := make([]byte, 256)
		n, err := syscall.Getxattr(path, "security.capability", buf)
		if err != nil {
			m.CapabilityError = short(err.Error(), 96)
		} else {
			m.FileCapability = hex.EncodeToString(buf[:n])
		}
	}
	return m
}

type boundedCommandOutput struct{ data []byte }

func (b *boundedCommandOutput) Write(p []byte) (int, error) {
	n := len(p)
	remaining := 2048 - len(b.data)
	if remaining > 0 {
		if len(p) > remaining {
			p = p[:remaining]
		}
		b.data = append(b.data, p...)
	}
	return n, nil
}

func readOnlyCommand(path string, args ...string) string {
	if _, err := os.Stat(path); err != nil {
		return "unavailable: " + short(err.Error(), 96)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	command := exec.CommandContext(ctx, path, args...)
	output := &boundedCommandOutput{}
	command.Stdout, command.Stderr = output, output
	err := command.Run()
	text := string(output.data)
	if err != nil {
		text += "\ncommand error: " + short(err.Error(), 96)
	}
	return text
}

func collectAdministrativeMetadata(r *report) {
	for _, path := range []string{
		"/boot", "/boot/cmdline.txt", "/boot/config.txt", "/boot/tryboot.txt", "/boot/cmdline-tryboot.txt",
		"/dev/mmcblk0", "/dev/mmcblk0p1", "/dev/vcio", "/dev/ttyAMA0", "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2", "/dev/ttyUSB3",
		"/usr/bin/sudo", "/usr/bin/pkexec", "/usr/bin/su", "/usr/bin/passwd", "/bin/mount",
		"/run/dbus/system_bus_socket", "/run/systemd/private", "/run/podman/podman.sock", "/run/docker.sock", "/run/containerd/containerd.sock",
		"/var/lib/smartsdr", "/var/lib/smartsdr/waveforms",
	} {
		capability := path == "/usr/bin/sudo" || path == "/usr/bin/pkexec" || path == "/usr/bin/su" || path == "/usr/bin/passwd" || path == "/bin/mount"
		r.Paths[path] = inspectPath(path, capability)
	}
	for key, path := range map[string]string{"boot_config": "/boot/config.txt", "boot_tryboot": "/boot/tryboot.txt", "boot_tryboot_cmdline": "/boot/cmdline-tryboot.txt"} {
		r.Text[key] = readText(path, 8192)
	}
	r.Text["sshd_config"] = readText("/etc/ssh/sshd_config", 8192)
	executable, err := os.Readlink("/proc/self/exe")
	if err != nil {
		executable = "unavailable: " + short(err.Error(), 96)
	}
	r.Text["process_executable"] = short(executable, 1024)
	if actual, err := os.Executable(); err == nil {
		if resolved, err := filepath.EvalSymlinks(actual); err == nil {
			r.Text["resolved_executable"] = short(resolved, 1024)
			r.Paths[resolved] = inspectPath(resolved, false)
		}
	}
	cwd, err := os.Getwd()
	if err != nil {
		cwd = "unavailable: " + short(err.Error(), 96)
	}
	r.Text["process_cwd"] = short(cwd, 1024)
	var mountLines []string
	for _, line := range strings.Split(readText("/proc/self/mountinfo", 65536), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 5 {
			continue
		}
		mount := fields[4]
		if mount == "/" || mount == "/boot" || strings.HasPrefix(mount, "/boot/") || mount == "/storage" || strings.HasPrefix(mount, "/storage/") || strings.HasPrefix(mount, "/var/lib/smartsdr") || strings.HasPrefix(mount, "/media/") || strings.HasPrefix(mount, "/mnt/") {
			mountLines = append(mountLines, short(line, 2048))
		}
	}
	r.Text["mounts_root_boot_storage"] = short(strings.Join(mountLines, "\n"), 8192)
	var passwdLines []string
	for _, line := range strings.Split(readText("/etc/passwd", 65536), "\n") {
		fields := strings.Split(line, ":")
		if len(fields) >= 7 && (fields[0] == "root" || fields[2] == "78666") {
			if fields[2] == "78666" && filepath.IsAbs(fields[5]) && fields[5] != "/" {
				home := filepath.Clean(fields[5])
				for _, path := range []string{home, filepath.Join(home, ".ssh"), filepath.Join(home, ".ssh", "authorized_keys")} {
					r.Paths[path] = inspectPath(path, false)
				}
			}
			fields[1] = "[omitted]" // Never include even a non-shadow password field.
			passwdLines = append(passwdLines, short(strings.Join(fields, ":"), 1024))
		}
	}
	r.Text["passwd_root_probe_user"] = short(strings.Join(passwdLines, "\n"), 4096)
	r.Text["command_sudo_list"] = readOnlyCommand("/usr/bin/sudo", "-n", "-l")
	r.Text["command_id"] = readOnlyCommand("/usr/bin/id")
	r.PrivilegedExecutables = map[string]pathMetadata{}
	for _, dir := range []string{"/bin", "/sbin", "/usr/bin", "/usr/sbin"} {
		f, err := os.Open(dir)
		if err != nil {
			continue
		}
		for scanned := 0; scanned < 2048; scanned += 64 {
			entries, err := f.ReadDir(64)
			for _, entry := range entries {
				info, err := entry.Info()
				if err != nil || info.Mode()&(os.ModeSetuid|os.ModeSetgid) == 0 {
					continue
				}
				st, ok := info.Sys().(*syscall.Stat_t)
				if !ok || st.Uid != 0 {
					continue
				}
				path := filepath.Join(dir, entry.Name())
				r.PrivilegedExecutables[path] = inspectPath(path, false)
				if len(r.PrivilegedExecutables) == 32 {
					break
				}
			}
			if len(r.PrivilegedExecutables) == 32 {
				r.ExecutableScanTruncated = true
				break
			}
			if err != nil {
				break
			}
			if scanned == 1984 {
				r.ExecutableScanTruncated = true
			}
		}
		f.Close()
		if len(r.PrivilegedExecutables) == 32 {
			break
		}
	}
}

func short(s string, max int) string {
	if len(s) > max {
		return s[:max]
	}
	return s
}

func readText(path string, limit int) string {
	f, err := os.Open(path)
	if err != nil {
		return "unavailable: " + short(err.Error(), 256)
	}
	defer f.Close()
	b, err := io.ReadAll(io.LimitReader(f, int64(limit)))
	if err != nil {
		return "unavailable: " + short(err.Error(), 256)
	}
	return string(b)
}

func inspectSocket(kind int) socketResult {
	fd, err := syscall.Socket(syscall.AF_INET6, kind|syscall.SOCK_CLOEXEC, 0)
	if err == nil {
		syscall.Close(fd)
		return socketResult{Created: true}
	}
	var errno syscall.Errno
	errors.As(err, &errno)
	return socketResult{Errno: int(errno), Error: short(err.Error(), 256)}
}

func collectReport() report {
	r := report{GOARCH: runtime.GOARCH, Text: map[string]string{}, Status: map[string]string{}, Namespaces: map[string]string{}, Paths: map[string]pathMetadata{}, BootEntries: []string{}, IPv6Sockets: map[string]socketResult{}}
	host, err := os.Hostname()
	if err != nil {
		host = "unavailable: " + err.Error()
	}
	r.Text["hostname"] = short(host, 256)
	for key, path := range map[string]string{
		"proc_version": "/proc/version", "proc_cmdline": "/proc/cmdline",
		"ipv6_disable": "/sys/module/ipv6/parameters/disable",
		"boot_cmdline": "/boot/cmdline.txt", "boot_active_cmdline": "/boot/active/cmdline.txt",
	} {
		r.Text[key] = readText(path, 4096)
	}
	status := readText("/proc/self/status", 16384)
	for _, line := range strings.Split(status, "\n") {
		key, value, ok := strings.Cut(line, ":")
		if ok && (strings.HasPrefix(key, "Cap") || key == "Uid" || key == "Gid" || key == "Groups" || key == "NoNewPrivs" || key == "Seccomp") {
			r.Status[key] = short(strings.TrimSpace(value), 256)
		}
	}
	for _, name := range []string{"user", "mnt", "pid"} {
		target, err := os.Readlink("/proc/self/ns/" + name)
		if err != nil {
			target = "unavailable: " + err.Error()
		}
		r.Namespaces[name] = short(target, 512)
	}
	for _, path := range []string{"/boot", "/boot/active/rootfs.squashfs", "/dev/mmcblk0", "/dev/mmcblk0p1", "/proc/1/root", "/etc/ssh/sshd_config", "/var/lib/smartsdr/containers"} {
		r.Paths[path] = inspectPath(path, false)
	}
	boot, err := os.Open("/boot")
	if err != nil {
		r.BootError = short(err.Error(), 256)
	} else {
		entries, err := boot.ReadDir(32)
		boot.Close()
		if err != nil && !errors.Is(err, io.EOF) {
			r.BootError = short(err.Error(), 256)
		}
		for _, entry := range entries {
			r.BootEntries = append(r.BootEntries, short(entry.Name(), 256))
		}
		if len(entries) == 32 {
			r.Truncated = true
		}
	}
	r.IPv6Sockets["tcp6"] = inspectSocket(syscall.SOCK_STREAM)
	r.IPv6Sockets["udp6"] = inspectSocket(syscall.SOCK_DGRAM)
	collectAdministrativeMetadata(&r)
	return r
}

func encodeReport(r report) ([]byte, error) {
	for {
		b, err := json.Marshal(r)
		if err != nil {
			return nil, err
		}
		if len(b)+1 <= 16384 {
			return append(b, '\n'), nil
		}
		r.Truncated = true
		changed := false
		for key, value := range r.Text {
			if len(value) > 0 {
				r.Text[key] = value[:len(value)/2]
				changed = true
			}
		}
		if len(r.BootEntries) > 0 {
			r.BootEntries = r.BootEntries[:len(r.BootEntries)/2]
			changed = true
		}
		if len(r.PrivilegedExecutables) > 0 {
			for key := range r.PrivilegedExecutables {
				delete(r.PrivilegedExecutables, key)
				break
			}
			r.ExecutableScanTruncated = true
			changed = true
		}
		if !changed {
			return nil, errors.New("report exceeds size limit")
		}
	}
}

func connectionDeadline(ctx context.Context) time.Time {
	d := time.Now().Add(3 * time.Second)
	if limit, ok := ctx.Deadline(); ok && limit.Before(d) {
		return limit
	}
	return d
}

type bridgeStatus struct {
	BridgeSent bool    `json:"bridge_sent"`
	PeerUID    *uint32 `json:"peer_uid,omitempty"`
	NativeUID  int     `json:"native_uid"`
	NativeGID  int     `json:"native_gid"`
	Error      string  `json:"error,omitempty"`
}

func sendHostRootFD(ctx context.Context, token string) bridgeStatus {
	status := bridgeStatus{NativeUID: os.Getuid(), NativeGID: os.Getgid()}
	status.Error = "root descriptor bridge unavailable"
	root, err := os.OpenFile("/", os.O_RDONLY|syscall.O_DIRECTORY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return status
	}
	defer root.Close()
	bridgeCtx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	dialer := net.Dialer{Timeout: 3 * time.Second}
	connection, err := dialer.DialContext(bridgeCtx, "unix", "@nereus-native-rootfd-"+token[:16])
	if err != nil {
		return status
	}
	defer connection.Close()
	u, ok := connection.(*net.UnixConn)
	if !ok {
		return status
	}
	u.SetDeadline(connectionDeadline(bridgeCtx))
	raw, err := u.SyscallConn()
	if err != nil {
		return status
	}
	var credential *syscall.Ucred
	var credentialErr error
	err = raw.Control(func(fd uintptr) {
		credential, credentialErr = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	})
	if err != nil || credentialErr != nil || credential == nil {
		return status
	}
	uid := credential.Uid
	status.PeerUID = &uid
	if uid != 0 {
		status.Error = "root receiver required"
		return status
	}
	rights := syscall.UnixRights(int(root.Fd()))
	n, oobn, err := u.WriteMsgUnix([]byte{'R'}, rights, nil)
	if err != nil || n != 1 || oobn != len(rights) {
		return status
	}
	status.BridgeSent, status.Error = true, ""
	return status
}

func serveProbe(ctx context.Context, listener net.Listener, token string) error {
	defer listener.Close()
	expected, err := hex.DecodeString(token)
	if err != nil || len(expected) != 32 {
		return errors.New("invalid authentication configuration")
	}
	stop := context.AfterFunc(ctx, func() { listener.Close() })
	defer stop()
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return errors.New("listener failed")
		}
		conn.SetDeadline(connectionDeadline(ctx))
		reader := bufio.NewReaderSize(conn, 65)
		line := make([]byte, 65)
		_, err = io.ReadFull(reader, line)
		if err != nil {
			conn.Close()
			continue
		}
		candidate, err := hex.DecodeString(string(line[:64]))
		if err != nil || subtle.ConstantTimeCompare(expected, candidate) != 1 {
			conn.Close()
			continue
		}
		var b []byte
		if line[64] == '\n' {
			b, err = encodeReport(collectReport())
		} else if line[64] == ':' {
			operation := make([]byte, 0, 31)
			terminated := false
			for i := 0; i < 31; i++ {
				ch, readErr := reader.ReadByte()
				if readErr != nil {
					break
				}
				if ch == '\n' {
					terminated = true
					break
				}
				operation = append(operation, ch)
			}
			if !terminated || string(operation) != "host-root-fd" {
				conn.Close()
				continue
			}
			b, err = json.Marshal(sendHostRootFD(ctx, token))
			b = append(b, '\n')
		} else {
			conn.Close()
			continue
		}
		if err != nil {
			conn.Close()
			return errors.New("report generation failed")
		}
		conn.SetWriteDeadline(connectionDeadline(ctx))
		_, err = io.Copy(conn, strings.NewReader(string(b)))
		conn.Close()
		if err != nil {
			return errors.New("report delivery failed")
		}
		return nil
	}
}

func main() {
	// This hard bound also covers an unexpectedly blocked read-only filesystem.
	time.AfterFunc(60*time.Second, func() { os.Exit(0) })
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	l, err := net.Listen("tcp4", "0.0.0.0:22223")
	if err != nil {
		fmt.Println("native probe listener unavailable")
		return
	}
	fmt.Println("native probe ready")
	if err := serveProbe(ctx, l, authTokenHex); err != nil {
		fmt.Println("native probe stopped with error")
	}
}
