package com.mqvpn.sdk.network

import android.net.Network
import android.os.ParcelFileDescriptor
import android.util.Log
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketAddress

/**
 * Creates UDP sockets bound to a specific [Network] and returns a raw fd
 * with ownership transferred to the caller.
 *
 * Important design constraints:
 * - Uses [Network.bindSocket] (NOT protect()) for VPN bypass + network pinning.
 *   bindSocket() and protect() must not both be called — they conflict on some devices.
 *   When network is null, falls back to protect().
 * - Sockets must NOT be in connected state (no DatagramSocket.connect()).
 *   libmqvpn uses sendto() with explicit peer address; connected sockets
 *   return EISCONN on sendto() with an address.
 */
object PathBinder {

    private const val TAG = "PathBinder"

    /**
     * Create a UDP socket bound to [network], returning a detached raw fd.
     *
     * FD ownership transfer (8 steps):
     * 1. DatagramSocket(null) — socket owns fd1
     * 2. network.bindSocket(socket) — kernel sets SO_MARK
     * 3. socket.bind(ephemeral port)
     * 4. ParcelFileDescriptor.fromDatagramSocket — pfd owns fd1
     * 5. pfd.dup() → dupPfd with fd2
     * 6. dupPfd.detachFd() → rawFd (caller owns fd2)
     * 7. pfd.close() — closes fd1
     * 8. return rawFd
     *
     * @param network Network to bind to, or null for default network.
     * @param protector VpnService.protect() callback, used only when network is null.
     * @return Raw fd (caller must close with Os.close), or -1 on error.
     */
    fun bindAndDetachUdp(
        network: Network?,
        remoteHost: String,
        remotePort: Int,
        protector: ((Int) -> Boolean)? = null,
    ): Int {
        return try {
            doBindAndDetach(network, protector)
        } catch (e: Exception) {
            Log.e(TAG, "bindAndDetachUdp failed: ${e.message}", e)
            -1
        }
    }

    /**
     * Create a UDP socket with a specific local address.
     * AF is determined from [localAddr].
     *
     * @return Raw fd (caller must close with Os.close), or -1 on error.
     */
    fun bindAndDetachUdpByLocalAddr(
        localAddr: InetAddress,
        remoteHost: String,
        remotePort: Int,
        protector: ((Int) -> Boolean)? = null,
        interfaceName: String? = null,
    ): Int {
        return try {
            doBindAndDetachLocal(localAddr, protector)
        } catch (e: Exception) {
            Log.e(TAG, "bindAndDetachUdpByLocalAddr failed: ${e.message}", e)
            -1
        }
    }

    private fun doBindAndDetach(
        network: Network?,
        protector: ((Int) -> Boolean)?,
    ): Int {
        // Step 1: Create unbound socket
        val sock = DatagramSocket(null as SocketAddress?)

        // Step 2: Bind to network (VPN bypass + network pinning)
        if (network != null) {
            network.bindSocket(sock)
        }

        // Step 3: Bind to ephemeral port
        sock.bind(InetSocketAddress(0))

        // Step 4: Transfer fd ownership to ParcelFileDescriptor
        val pfd = ParcelFileDescriptor.fromDatagramSocket(sock)

        // If network is null, use protect() for VPN bypass
        if (network == null && protector != null) {
            protector(pfd.fd)
        }

        // Steps 5-6: Dup and detach
        val dupPfd = pfd.dup()
        val rawFd = dupPfd.detachFd()

        // Step 7: Close originals
        pfd.close()
        // sock is already invalidated by fromDatagramSocket; close is safe (no-op)
        sock.close()

        // Step 8: Return raw fd
        return rawFd
    }

    private fun doBindAndDetachLocal(
        localAddr: InetAddress,
        protector: ((Int) -> Boolean)?,
    ): Int {
        val sock = DatagramSocket(null as SocketAddress?)
        sock.bind(InetSocketAddress(localAddr, 0))

        val pfd = ParcelFileDescriptor.fromDatagramSocket(sock)

        if (protector != null) {
            protector(pfd.fd)
        }

        val dupPfd = pfd.dup()
        val rawFd = dupPfd.detachFd()

        pfd.close()
        sock.close()

        return rawFd
    }
}
