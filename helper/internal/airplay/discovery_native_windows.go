//go:build windows

package airplay

import (
	"context"
	"fmt"
	"net"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// Native DNS-SD discovery through the Windows DNS client service (dnsapi.dll,
// Windows 10 1709 and later). That service shares UDP 5353 with Bonjour and
// other mDNS software, where the built-in zeroconf listener only sometimes
// receives replies (five processes on port 5353 on the church streaming PC,
// 2026-09-26).
//
// Per scan: DnsServiceBrowse("_airplay._tcp.local") delivers PTR records that
// name service instances; each new instance is resolved with
// DnsServiceResolve, whose DNS_SERVICE_INSTANCE carries the address, port and
// TXT properties. Everything outstanding is cancelled when ctx ends.

const (
	dnsQueryRequestVersion1 = 1 // DNS_QUERY_REQUEST_VERSION1
	dnsFreeRecordList       = 1 // DNS_FREE_TYPE DnsFreeRecordList
	airplayBrowseName       = "_airplay._tcp.local"
)

var (
	modDNSAPI                   = windows.NewLazySystemDLL("dnsapi.dll")
	procDnsServiceBrowse        = modDNSAPI.NewProc("DnsServiceBrowse")
	procDnsServiceBrowseCancel  = modDNSAPI.NewProc("DnsServiceBrowseCancel")
	procDnsServiceResolve       = modDNSAPI.NewProc("DnsServiceResolve")
	procDnsServiceResolveCancel = modDNSAPI.NewProc("DnsServiceResolveCancel")
	procDnsServiceFreeInstance  = modDNSAPI.NewProc("DnsServiceFreeInstance")
)

// The trampolines are created once: syscall.NewCallback slots are never
// released and a process can create only a limited number.
var (
	callbacksOnce      sync.Once
	browseCallbackPtr  uintptr
	resolveCallbackPtr uintptr
)

// The OS gets an integer key as its query context, never a Go pointer.
var (
	nativeContextsMu sync.Mutex
	nativeContexts   = map[uintptr]*nativeScan{}
	nextNativeKey    uintptr
)

type nativeScan struct {
	names   chan string        // instance FQDNs from browse PTR records
	results chan resolveResult // resolve completions
}

type resolveResult struct {
	key uintptr
	svc *dnssdService // nil when the resolve failed
}

func registerNativeContext(s *nativeScan) uintptr {
	nativeContextsMu.Lock()
	defer nativeContextsMu.Unlock()
	nextNativeKey++
	nativeContexts[nextNativeKey] = s
	return nextNativeKey
}

func unregisterNativeContext(key uintptr) {
	nativeContextsMu.Lock()
	delete(nativeContexts, key)
	nativeContextsMu.Unlock()
}

func lookupNativeContext(key uintptr) *nativeScan {
	nativeContextsMu.Lock()
	defer nativeContextsMu.Unlock()
	return nativeContexts[key]
}

// takeNativeContext looks a key up and unregisters it in one step. A resolve
// completes exactly once, so its callback takes its key; cleanup then cancels
// only resolves whose key is still registered, never one that has completed
// (the docs do not say that cancelling a finished resolve is safe).
func takeNativeContext(key uintptr) *nativeScan {
	nativeContextsMu.Lock()
	defer nativeContextsMu.Unlock()
	s := nativeContexts[key]
	delete(nativeContexts, key)
	return s
}

// nativeDiscoveryAvailable reports whether this Windows build exports the
// DNS-SD functions (Windows 10 1709 and later).
func nativeDiscoveryAvailable() error {
	for _, p := range []*windows.LazyProc{
		procDnsServiceBrowse, procDnsServiceBrowseCancel,
		procDnsServiceResolve, procDnsServiceResolveCancel, procDnsServiceFreeInstance,
	} {
		if err := p.Find(); err != nil {
			return fmt.Errorf("%w: %v", errNativeDiscoveryUnavailable, err)
		}
	}
	return nil
}

// onBrowse is the DNS_SERVICE_BROWSE_CALLBACK. It runs on a system thread and
// must not block.
func onBrowse(status uint32, key uintptr, records *windows.DNSRecord) uintptr {
	if records == nil {
		return 0
	}
	defer windows.DnsRecordListFree(records, dnsFreeRecordList)
	scan := lookupNativeContext(key)
	if scan == nil || status != 0 {
		return 0
	}
	for r := records; r != nil; r = r.Next {
		if r.Type != windows.DNS_TYPE_PTR || r.Ttl == 0 {
			continue // not an instance name, or a goodbye
		}
		// DNS_PTR_DATAW is { PWSTR pNameHost }, the first member of Data.
		host := *(**uint16)(unsafe.Pointer(&r.Data[0]))
		if host == nil {
			continue
		}
		select {
		case scan.names <- windows.UTF16PtrToString(host):
		default: // the scan is behind or over; the next scan sees it again
		}
	}
	return 0
}

// onResolve is the DNS_SERVICE_RESOLVE_COMPLETE callback. It runs once per
// resolve on a system thread, takes the resolve's key and must not block.
func onResolve(status uint32, key uintptr, inst *dnsServiceInstance) uintptr {
	var svc *dnssdService
	if inst != nil {
		if status == 0 {
			copied := copyServiceInstance(inst)
			svc = &copied
		}
		procDnsServiceFreeInstance.Call(uintptr(unsafe.Pointer(inst)))
	}
	if scan := takeNativeContext(key); scan != nil {
		select {
		case scan.results <- resolveResult{key: key, svc: svc}:
		default:
		}
	}
	return 0
}

// copyServiceInstance copies an OS-owned DNS_SERVICE_INSTANCE into Go memory.
func copyServiceInstance(inst *dnsServiceInstance) dnssdService {
	svc := dnssdService{
		InstanceName: windows.UTF16PtrToString(inst.InstanceName),
		HostName:     windows.UTF16PtrToString(inst.HostName),
		Port:         inst.Port,
	}
	if inst.IP4Address != nil {
		svc.IPv4 = ipv4FromIP4Address(*(*[4]byte)(unsafe.Pointer(inst.IP4Address)))
	}
	if inst.IP6Address != nil {
		svc.IPv6 = net.IP(append([]byte(nil), inst.IP6Address[:]...))
	}
	if n := int(inst.PropertyCount); n > 0 && inst.Keys != nil && inst.Values != nil {
		keys := unsafe.Slice(inst.Keys, n)
		values := unsafe.Slice(inst.Values, n)
		for i := 0; i < n; i++ {
			svc.Keys = append(svc.Keys, windows.UTF16PtrToString(keys[i]))
			svc.Values = append(svc.Values, windows.UTF16PtrToString(values[i]))
		}
	}
	return svc
}

// discoverNative browses _airplay._tcp through the Windows DNS-SD API until
// ctx ends. Errors are returned only before browsing starts, so the caller can
// fall back to zeroconf with the whole scan window left.
func discoverNative(ctx context.Context) ([]AirPlayDevice, error) {
	if err := nativeDiscoveryAvailable(); err != nil {
		return nil, err
	}
	callbacksOnce.Do(func() {
		browseCallbackPtr = syscall.NewCallback(onBrowse)
		resolveCallbackPtr = syscall.NewCallback(onResolve)
	})

	scan := &nativeScan{names: make(chan string, 256), results: make(chan resolveResult, 256)}
	browseKey := registerNativeContext(scan)
	defer unregisterNativeContext(browseKey)

	// The OS holds the request, cancel and name buffers until the operation is
	// cancelled; pin them so they neither move nor get collected.
	var pinner runtime.Pinner
	defer pinner.Unpin()

	queryName, err := windows.UTF16PtrFromString(airplayBrowseName)
	if err != nil {
		return nil, err
	}
	browseReq := &dnsServiceBrowseRequest{
		Version:        dnsQueryRequestVersion1,
		QueryName:      queryName,
		BrowseCallback: browseCallbackPtr,
		QueryContext:   browseKey,
	}
	browseCancel := &dnsServiceCancel{}
	pinner.Pin(queryName)
	pinner.Pin(browseReq)
	pinner.Pin(browseCancel)

	status, _, _ := procDnsServiceBrowse.Call(uintptr(unsafe.Pointer(browseReq)), uintptr(unsafe.Pointer(browseCancel)))
	if syscall.Errno(status) != windows.DNS_REQUEST_PENDING {
		return nil, fmt.Errorf("DnsServiceBrowse(%s): %w", airplayBrowseName, syscall.Errno(status))
	}
	defer procDnsServiceBrowseCancel.Call(uintptr(unsafe.Pointer(browseCancel)))

	inflight := make(map[uintptr]*dnsServiceCancel)
	defer func() {
		for key, cancel := range inflight {
			if takeNativeContext(key) != nil { // not completed yet
				procDnsServiceResolveCancel.Call(uintptr(unsafe.Pointer(cancel)))
			}
		}
	}()

	resolve := func(fqdn string) {
		name, err := windows.UTF16PtrFromString(fqdn)
		if err != nil {
			return
		}
		key := registerNativeContext(scan)
		req := &dnsServiceResolveRequest{
			Version:                   dnsQueryRequestVersion1,
			QueryName:                 name,
			ResolveCompletionCallback: resolveCallbackPtr,
			QueryContext:              key,
		}
		cancel := &dnsServiceCancel{}
		pinner.Pin(name)
		pinner.Pin(req)
		pinner.Pin(cancel)
		st, _, _ := procDnsServiceResolve.Call(uintptr(unsafe.Pointer(req)), uintptr(unsafe.Pointer(cancel)))
		if syscall.Errno(st) != windows.DNS_REQUEST_PENDING {
			unregisterNativeContext(key)
			dbg("[discovery] DnsServiceResolve(%s): %v", fqdn, syscall.Errno(st))
			return
		}
		inflight[key] = cancel
	}

	seen := make(map[string]bool)
	var devices []AirPlayDevice
	for {
		select {
		case <-ctx.Done():
			return devices, nil
		case fqdn := <-scan.names:
			key := strings.ToLower(fqdn)
			if seen[key] {
				continue
			}
			seen[key] = true
			resolve(fqdn)
		case res := <-scan.results:
			if _, ok := inflight[res.key]; !ok {
				continue
			}
			delete(inflight, res.key) // onResolve already took the key
			if res.svc == nil {
				continue
			}
			if dev := deviceFromDNSSD(*res.svc); dev != nil {
				devices = append(devices, *dev)
			} else {
				dbg("[discovery] %s resolved without a usable address", res.svc.InstanceName)
			}
		}
	}
}
