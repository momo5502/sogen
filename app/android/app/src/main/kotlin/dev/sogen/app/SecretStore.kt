package dev.sogen.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.security.GeneralSecurityException
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Credentials encrypted with a key that never leaves the Android Keystore. They live in the no-backup directory, so
 * a restored backup can never carry a credential whose key stayed on the old device.
 */
class SecretStore(context: Context) {
    private val directory = File(context.noBackupFilesDir, "secrets")

    /** The secret stored as [name], or null when there is none or it can no longer be decrypted. */
    fun read(name: String): String? {
        val file = fileOf(name)
        if (!file.exists()) {
            return null
        }

        return try {
            decrypt(name, file.readBytes())
        } catch (e: GeneralSecurityException) {
            Log.w(TAG, "Discarding the secret $name, which can no longer be decrypted: ${e.javaClass.simpleName}")
            file.delete()
            null
        }
    }

    fun write(name: String, value: String) {
        val file = fileOf(name)
        directory.mkdirs()

        val staging = File(directory, "${file.name}.staging")
        FileOutputStream(staging).use { stream ->
            stream.write(encrypt(name, value))
            stream.fd.sync()
        }

        if (!staging.renameTo(file)) {
            staging.delete()
            throw IOException("Could not store the secret $name")
        }
    }

    fun delete(name: String) {
        val file = fileOf(name)
        if (!file.delete() && file.exists()) {
            throw IOException("Could not delete the secret $name")
        }
    }

    private fun fileOf(name: String): File {
        require(name.matches(validName)) { "Invalid secret name" }
        return File(directory, name)
    }

    private fun encrypt(name: String, value: String): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        cipher.updateAAD(name.toByteArray())
        val nonce = cipher.iv
        check(nonce.size == NONCE_SIZE)
        return byteArrayOf(FORMAT_VERSION) + nonce + cipher.doFinal(value.toByteArray())
    }

    private fun decrypt(name: String, envelope: ByteArray): String {
        if (envelope.size < 1 + NONCE_SIZE || envelope[0] != FORMAT_VERSION) {
            throw GeneralSecurityException("Unknown secret format")
        }

        val key = existingKey() ?: throw GeneralSecurityException("The key is gone")
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(TAG_BITS, envelope, 1, NONCE_SIZE))
        cipher.updateAAD(name.toByteArray())
        return String(cipher.doFinal(envelope, 1 + NONCE_SIZE, envelope.size - 1 - NONCE_SIZE))
    }

    private fun existingKey(): SecretKey? {
        val keyStore = KeyStore.getInstance(KEYSTORE).apply { load(null) }
        return keyStore.getKey(KEY_ALIAS, null) as SecretKey?
    }

    private fun key(): SecretKey = existingKey() ?: KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE).run {
        init(
            KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        generateKey()
    }

    private companion object {
        const val TAG = "SecretStore"
        const val KEYSTORE = "AndroidKeyStore"
        const val KEY_ALIAS = "sogen-secrets"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val FORMAT_VERSION: Byte = 1
        const val NONCE_SIZE = 12
        const val TAG_BITS = 128

        val validName = Regex("[a-z][a-z0-9_]*")
    }
}
