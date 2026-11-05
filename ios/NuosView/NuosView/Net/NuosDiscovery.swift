//
//  NuosDiscovery.swift
//  NuosView
//
//  mDNS browse for the board's NuOS control service: "_nuos-xr._tcp"
//  (advertised on TCP port 48012, TXT: board=nuos, proto=1).
//

import Foundation

struct NuosDiscoveredBoard: Identifiable, Equatable {
    /// Stable identity: "\(name).\(type).\(domain)".
    let id: String
    var name: String
    var host: String?
    var port: Int
    var txt: [String: String]
    /// Address endpoints usable for NWConnection (resolved, may be empty).
    var endpoints: [NWEndpoint] = []

    var protoVersion: Int? { txt["proto"].flatMap(Int.init) }
    var isNuosBoard: Bool { txt["board"] == "nuos" }
}

/// Browses `_nuos-xr._tcp` via NSNetServiceBrowser and resolves each result
/// (host, port, TXT) so the UI can offer a connect action.
final class NuosDiscovery: NSObject, ObservableObject {
    @Published var boards: [NuosDiscoveredBoard] = []
    @Published var isBrowsing = false
    @Published var lastError: String?

    private var browser: NetServiceBrowser?
    private var services: [NetService] = []
    private let queue = DispatchQueue.main

    func start() {
        stop()
        let b = NetServiceBrowser()
        browser = b
        b.delegate = self
        b.searchForServices(ofType: NuosProto.mdnsServiceType + ".", inDomain: "local.")
        isBrowsing = true
    }

    func stop() {
        browser?.stop()
        browser = nil
        for s in services { s.stop() }
        services.removeAll()
        boards.removeAll()
        isBrowsing = false
    }

    private func upsert(_ service: NetService) {
        queue.async { [weak self] in
            guard let self else { return }
            if !self.services.contains(service) {
                self.services.append(service)
                service.delegate = self
                service.resolve(withTimeout: 5)
            }
        }
    }

    private func rebuildBoards() {
        boards = services.compactMap { service -> NuosDiscoveredBoard? in
            var txt: [String: String] = [:]
            if let records = service.TXTRecordData {
                let dict = NetService.dictionary(fromTXTRecord: records)
                for (k, v) in dict {
                    if let s = v as? Data { txt[k] = String(data: s, encoding: .utf8) ?? "" }
                    else if let s = v as? String { txt[k] = s }
                }
            }
            var endpoints: [NWEndpoint] = []
            var host: String?
            // service.addresses is [Data] of sockaddr blobs; convert to
            // NWEndpoint so NuosLink can connect without re-resolving names.
            for data in service.addresses ?? [] {
                if let ep = Self.nwEndpoint(fromSockaddr: data) {
                    endpoints.append(ep)
                }
            }
            if host == nil { host = service.hostName }
            let id = "\(service.name).\(service.type).\(service.domain)"
            return NuosDiscoveredBoard(
                id: id,
                name: service.name,
                host: host,
                port: service.port,
                txt: txt,
                endpoints: endpoints)
        }
    }

    /// Build an NWEndpoint from a sockaddr_in data blob.
    static func nwEndpoint(fromSockaddr data: Data) -> NWEndpoint? {
        data.withUnsafeBytes { raw -> NWEndpoint? in
            guard raw.count >= MemoryLayout<sockaddr_in>.size else { return nil }
            let sa = raw.bindMemory(to: sockaddr.self).first!
            if sa.sa_family == sa_family_t(AF_INET) {
                let sin = raw.bindMemory(to: sockaddr_in.self).first!
                let addrBytes = withUnsafeBytes(of: sin.sin_addr) { Data($0) }
                guard let addr = IPv4Address(addrBytes) else { return nil }
                guard let port = NWEndpoint.Port(rawValue: UInt16(bigEndian: sin.sin_port)) else { return nil }
                return NWEndpoint.hostPort(host: .ipv4(addr), port: port)
            } else if sa.sa_family == sa_family_t(AF_INET6), raw.count >= MemoryLayout<sockaddr_in6>.size {
                let sin6 = raw.bindMemory(to: sockaddr_in6.self).first!
                let addrBytes = withUnsafeBytes(of: sin6.sin6_addr) { Data($0) }
                guard let addr = IPv6Address(addrBytes) else { return nil }
                guard let port = NWEndpoint.Port(rawValue: UInt16(bigEndian: sin6.sin6_port)) else { return nil }
                return NWEndpoint.hostPort(host: .ipv6(addr), port: port)
            }
            return nil
        }
    }
}

extension NuosDiscovery: NetServiceBrowserDelegate {
    func netServiceBrowser(_ browser: NetServiceBrowser,
                           didFind service: NetService, moreComing: Bool) {
        upsert(service)
        if !moreComing { rebuildBoards() }
    }

    func netServiceBrowser(_ browser: NetServiceBrowser,
                           didRemove service: NetService, moreComing: Bool) {
        queue.async { [weak self] in
            guard let self else { return }
            self.services.removeAll { $0 == service }
            service.stop()
            if !moreComing { self.rebuildBoards() }
        }
    }

    func netServiceBrowser(_ browser: NetServiceBrowser,
                           didNotSearch errorDict: [String: NSNumber]) {
        queue.async { [weak self] in
            self?.isBrowsing = false
            self?.lastError = "browse failed: \(errorDict[NetService.errorCodeKey] ?? -1)"
        }
    }

    func netServiceBrowserDidStopSearch(_ browser: NetServiceBrowser) {
        queue.async { [weak self] in self?.isBrowsing = false }
    }
}

extension NuosDiscovery: NetServiceDelegate {
    func netServiceDidResolveAddress(_ service: NetService) {
        queue.async { [weak self] in self?.rebuildBoards() }
    }

    func netService(_ sender: NetService,
                    didNotResolve errorDict: [String: NSNumber]) {
        queue.async { [weak self] in
            self?.services.removeAll { $0 == sender }
            self?.rebuildBoards()
        }
    }

    func netServiceDidStop(_ sender: NetService) {
        queue.async { [weak self] in
            self?.services.removeAll { $0 == sender }
            self?.rebuildBoards()
        }
    }
}

import Network
