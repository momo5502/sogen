package dev.sogen.app.runtime

import android.os.Bundle

data class RunConfig(
    val emulationRoot: String,
    val application: String,
    val arguments: List<String> = emptyList(),
    val environment: Map<String, String> = emptyMap(),
    /** Guest Windows path to host path, e.g. `C:\game` to the folder the executable lives in. */
    val pathMappings: Map<String, String> = emptyMap(),
    /** Guest Windows path to a Storage Access Framework tree URI, mounted copy-on-write. */
    val documentMounts: Map<String, String> = emptyMap(),
    /** Where the writes to [documentMounts] are kept, one subdirectory per mount. */
    val layerDirectory: String = "",
    val workingDirectory: String = "",
    /** Empty selects the platform default: FEX on arm64, Unicorn elsewhere. */
    val backend: String = "",
    val liveNetwork: Boolean = false,
    /** Where a suspend without a destination saves the process. */
    val snapshotDirectory: String = "",
    /** A saved process to continue instead of starting [application]. */
    val resumeFile: String = "",
) {
    fun toBundle(): Bundle = Bundle().apply {
        putString(KEY_ROOT, emulationRoot)
        putString(KEY_APPLICATION, application)
        putStringArrayList(KEY_ARGUMENTS, ArrayList(arguments))
        putStringArrayList(KEY_ENVIRONMENT, ArrayList(environment.flatMap { listOf(it.key, it.value) }))
        putStringArrayList(KEY_PATH_MAPPINGS, ArrayList(pathMappings.flatMap { listOf(it.key, it.value) }))
        putStringArrayList(KEY_DOCUMENT_MOUNTS, ArrayList(documentMounts.flatMap { listOf(it.key, it.value) }))
        putString(KEY_LAYER_DIRECTORY, layerDirectory)
        putString(KEY_WORKING_DIRECTORY, workingDirectory)
        putString(KEY_BACKEND, backend)
        putBoolean(KEY_LIVE_NETWORK, liveNetwork)
        putString(KEY_SNAPSHOT_DIRECTORY, snapshotDirectory)
        putString(KEY_RESUME_FILE, resumeFile)
    }

    companion object {
        private const val KEY_ROOT = "root"
        private const val KEY_APPLICATION = "application"
        private const val KEY_ARGUMENTS = "arguments"
        private const val KEY_ENVIRONMENT = "environment"
        private const val KEY_PATH_MAPPINGS = "pathMappings"
        private const val KEY_DOCUMENT_MOUNTS = "documentMounts"
        private const val KEY_LAYER_DIRECTORY = "layerDirectory"
        private const val KEY_WORKING_DIRECTORY = "workingDirectory"
        private const val KEY_BACKEND = "backend"
        private const val KEY_LIVE_NETWORK = "liveNetwork"
        private const val KEY_SNAPSHOT_DIRECTORY = "snapshotDirectory"
        private const val KEY_RESUME_FILE = "resumeFile"

        fun fromBundle(bundle: Bundle) = RunConfig(
            emulationRoot = bundle.getString(KEY_ROOT).orEmpty(),
            application = bundle.getString(KEY_APPLICATION).orEmpty(),
            arguments = bundle.getStringArrayList(KEY_ARGUMENTS).orEmpty(),
            environment = pairs(bundle.getStringArrayList(KEY_ENVIRONMENT)),
            pathMappings = pairs(bundle.getStringArrayList(KEY_PATH_MAPPINGS)),
            documentMounts = pairs(bundle.getStringArrayList(KEY_DOCUMENT_MOUNTS)),
            layerDirectory = bundle.getString(KEY_LAYER_DIRECTORY).orEmpty(),
            workingDirectory = bundle.getString(KEY_WORKING_DIRECTORY).orEmpty(),
            backend = bundle.getString(KEY_BACKEND).orEmpty(),
            liveNetwork = bundle.getBoolean(KEY_LIVE_NETWORK),
            snapshotDirectory = bundle.getString(KEY_SNAPSHOT_DIRECTORY).orEmpty(),
            resumeFile = bundle.getString(KEY_RESUME_FILE).orEmpty(),
        )

        private fun pairs(flat: List<String>?): Map<String, String> =
            flat.orEmpty().chunked(2).filter { it.size == 2 }.associate { it[0] to it[1] }
    }
}
