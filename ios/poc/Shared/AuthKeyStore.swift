// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

import Foundation
import Security

/// The server auth key lives in one keychain item shared by the app and the
/// PacketTunnel extension, not in providerConfiguration (which the system
/// keeps in the VPN preferences in clear). The tunnel protocol's
/// `passwordReference` holds the item's persistent reference; the extension
/// resolves it at tunnel start.
///
/// Sharing needs a keychain access group both targets are entitled to:
/// project.yml gives both `keychain-access-groups =
/// $(AppIdentifierPrefix)com.mp0rta.mqvpnpoc.shared`, and the same expanded
/// string in each Info.plist as `MqvpnKeychainGroup`. The prefix comes from
/// the signing team (DEVELOPMENT_TEAM in Config.xcconfig). Without the key
/// (or with an unexpanded value) the item goes to the default access group,
/// which is the first entry of keychain-access-groups.
enum AuthKeyStore {
    struct Failure: Error, CustomStringConvertible {
        let status: OSStatus
        var description: String { "keychain error \(status)" }
    }

    private static let service = "com.mp0rta.mqvpnpoc.authkey"
    private static let account = "server"

    private static var accessGroup: String? {
        guard let g = Bundle.main.object(forInfoDictionaryKey: "MqvpnKeychainGroup") as? String,
              !g.isEmpty, !g.contains("$(") else { return nil }
        return g
    }

    private static func itemQuery() -> [String: Any] {
        var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: account]
        if let g = accessGroup { q[kSecAttrAccessGroup as String] = g }
        return q
    }

    /// Stores `key`, replacing the previous one, and returns the item's
    /// persistent reference for `NETunnelProviderProtocol.passwordReference`.
    /// An empty key deletes the item and returns nil (no Authorization header).
    static func save(_ key: String) throws -> Data? {
        let query = itemQuery()
        if key.isEmpty {
            let st = SecItemDelete(query as CFDictionary)
            guard st == errSecSuccess || st == errSecItemNotFound else { throw Failure(status: st) }
            return nil
        }
        // AfterFirstUnlock: an on-demand start after a reboot must still read it.
        let attrs: [String: Any] = [kSecValueData as String: Data(key.utf8),
                                    kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock]
        var st = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if st == errSecItemNotFound {
            var add = query
            for (k, v) in attrs { add[k] = v }
            st = SecItemAdd(add as CFDictionary, nil)
        }
        guard st == errSecSuccess else { throw Failure(status: st) }

        var refQuery = query
        refQuery[kSecReturnPersistentRef as String] = true
        refQuery[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        st = SecItemCopyMatching(refQuery as CFDictionary, &out)
        guard st == errSecSuccess, let ref = out as? Data else {
            throw Failure(status: st == errSecSuccess ? errSecDecode : st)
        }
        return ref
    }

    /// The key behind a `passwordReference`; nil when there is no reference.
    /// Throws when a reference is set but cannot be read: connecting without
    /// the key would only end in an authentication failure.
    static func load(persistentRef: Data?) throws -> String? {
        guard let ref = persistentRef else { return nil }
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecValuePersistentRef as String: ref,
                                    kSecReturnData as String: true,
                                    kSecMatchLimit as String: kSecMatchLimitOne]
        var out: CFTypeRef?
        let st = SecItemCopyMatching(query as CFDictionary, &out)
        guard st == errSecSuccess, let data = out as? Data,
              let key = String(data: data, encoding: .utf8) else {
            throw Failure(status: st == errSecSuccess ? errSecDecode : st)
        }
        return key
    }
}
