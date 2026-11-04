package dev.sogen.app.download

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test
import java.io.IOException
import java.net.URL

class HttpChunkFetcherTest {
    private val endpoints = mapOf(
        ChunkSource.GAME to listOf(CdnEndpoint("https://cdn.example/store{path}", emptyMap())),
    )

    @Test
    fun aRetryAfterTheNetworkTurnedMeteredIsNotSent() {
        var metered = false
        val attempts = mutableListOf<URL>()
        val fetcher = HttpChunkFetcher(
            endpoints,
            beforeAttempt = {
                if (metered) {
                    throw MeteredNetworkException()
                }
            },
            transport = { url, _ ->
                attempts.add(url)
                metered = true
                throw IOException("Wi-Fi dropped")
            },
        )

        assertThrows(MeteredNetworkException::class.java) { fetcher.fetch(ChunkSource.GAME, "ab".repeat(16), 10) }
        assertEquals(1, attempts.size)
    }

    @Test
    fun aFailedAttemptIsRetriedOnTheChunkPath() {
        val attempts = mutableListOf<URL>()
        val fetcher = HttpChunkFetcher(endpoints, transport = { url, _ ->
            attempts.add(url)
            if (attempts.size == 1) {
                throw IOException("Timed out")
            }
            ByteArray(10)
        })

        fetcher.fetch(ChunkSource.GAME, "ab".repeat(16), 10)
        assertEquals(listOf("https://cdn.example/store/ab/ab/${"ab".repeat(16)}"), attempts.map { it.toString() }.distinct())
        assertEquals(2, attempts.size)
    }
}
