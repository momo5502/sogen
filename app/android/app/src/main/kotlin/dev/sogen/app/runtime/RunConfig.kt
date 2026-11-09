package dev.sogen.app.runtime

import android.os.Bundle

data class RunConfig(
    val emulationRoot: String,
    val application: String,
    val arguments: List<String> = emptyList(),
    val environment: Map<String, String> = emptyMap(),
    /** Where the changes to the emulation root are kept; the root itself is only read. */
    val rootLayerDirectory: String = "",
    /** Guest Windows path to a Storage Access Framework tree URI, mounted copy-on-write. */
    val documentMounts: Map<String, String> = emptyMap(),
    /** Guest Windows path to a folder below the app's games directory, mounted copy-on-write. */
    val directoryMounts: Map<String, String> = emptyMap(),
    /** Where the writes to [documentMounts] and [directoryMounts] are kept, one subdirectory per mount. */
    val layerDirectory: String = "",
    val workingDirectory: String = "",
    /** Empty selects the platform default: FEX on arm64, Unicorn elsewhere. */
    val backend: String = "",
    val liveNetwork: Boolean = false,
    /** The largest display mode the guest sees, such as "1280x720", or empty for the emulator's default. */
    val maxDisplayMode: String = "",
    /** Where a suspend without a destination saves the process. */
    val snapshotDirectory: String = "",
    /** A saved process to continue instead of starting [application]. */
    val resumeFile: String = "",
    /** Registry files imported into a fresh session over [registryState]. */
    val registryFiles: List<String> = emptyList(),
    /** Like [registryFiles], but their strings refer to the guest's environment variables, such as %USERPROFILE%. */
    val expandedRegistryFiles: List<String> = emptyList(),
    /** Where the guest's registry changes are kept between sessions, or empty to keep none. */
    val registryState: String = "",
    /** What the parent's process manager passed to a child process, which then runs instead of [application]. */
    val managedArguments: List<String> = emptyList(),
) {
    /**
     * The settings a child process of this game runs with. It takes only what all of the game's processes share; the
     * parent passes what belongs to the process itself.
     */
    fun forChildProcess(arguments: List<String>) = RunConfig(
        emulationRoot = emulationRoot,
        application = "",
        rootLayerDirectory = rootLayerDirectory,
        documentMounts = documentMounts,
        directoryMounts = directoryMounts,
        layerDirectory = layerDirectory,
        backend = backend,
        liveNetwork = liveNetwork,
        maxDisplayMode = maxDisplayMode,
        managedArguments = arguments,
    )

    fun toBundle(): Bundle = Bundle().apply {
        putString(KEY_ROOT, emulationRoot)
        putString(KEY_APPLICATION, application)
        putStringArrayList(KEY_ARGUMENTS, ArrayList(arguments))
        putStringArrayList(KEY_ENVIRONMENT, ArrayList(environment.flatMap { listOf(it.key, it.value) }))
        putString(KEY_ROOT_LAYER_DIRECTORY, rootLayerDirectory)
        putStringArrayList(KEY_DOCUMENT_MOUNTS, ArrayList(documentMounts.flatMap { listOf(it.key, it.value) }))
        putStringArrayList(KEY_DIRECTORY_MOUNTS, ArrayList(directoryMounts.flatMap { listOf(it.key, it.value) }))
        putString(KEY_LAYER_DIRECTORY, layerDirectory)
        putString(KEY_WORKING_DIRECTORY, workingDirectory)
        putString(KEY_BACKEND, backend)
        putBoolean(KEY_LIVE_NETWORK, liveNetwork)
        putString(KEY_MAX_DISPLAY_MODE, maxDisplayMode)
        putString(KEY_SNAPSHOT_DIRECTORY, snapshotDirectory)
        putString(KEY_RESUME_FILE, resumeFile)
        putStringArrayList(KEY_REGISTRY_FILES, ArrayList(registryFiles))
        putStringArrayList(KEY_EXPANDED_REGISTRY_FILES, ArrayList(expandedRegistryFiles))
        putString(KEY_REGISTRY_STATE, registryState)
        putStringArrayList(KEY_MANAGED_ARGUMENTS, ArrayList(managedArguments))
    }

    companion object {
        private const val KEY_ROOT = "root"
        private const val KEY_APPLICATION = "application"
        private const val KEY_ARGUMENTS = "arguments"
        private const val KEY_ENVIRONMENT = "environment"
        private const val KEY_ROOT_LAYER_DIRECTORY = "rootLayerDirectory"
        private const val KEY_DOCUMENT_MOUNTS = "documentMounts"
        private const val KEY_DIRECTORY_MOUNTS = "directoryMounts"
        private const val KEY_LAYER_DIRECTORY = "layerDirectory"
        private const val KEY_WORKING_DIRECTORY = "workingDirectory"
        private const val KEY_BACKEND = "backend"
        private const val KEY_LIVE_NETWORK = "liveNetwork"
        private const val KEY_MAX_DISPLAY_MODE = "maxDisplayMode"
        private const val KEY_SNAPSHOT_DIRECTORY = "snapshotDirectory"
        private const val KEY_RESUME_FILE = "resumeFile"
        private const val KEY_REGISTRY_FILES = "registryFiles"
        private const val KEY_EXPANDED_REGISTRY_FILES = "expandedRegistryFiles"
        private const val KEY_REGISTRY_STATE = "registryState"
        private const val KEY_MANAGED_ARGUMENTS = "managedArguments"

        fun fromBundle(bundle: Bundle) = RunConfig(
            emulationRoot = bundle.getString(KEY_ROOT).orEmpty(),
            application = bundle.getString(KEY_APPLICATION).orEmpty(),
            arguments = bundle.getStringArrayList(KEY_ARGUMENTS).orEmpty(),
            environment = pairs(bundle.getStringArrayList(KEY_ENVIRONMENT)),
            rootLayerDirectory = bundle.getString(KEY_ROOT_LAYER_DIRECTORY).orEmpty(),
            documentMounts = pairs(bundle.getStringArrayList(KEY_DOCUMENT_MOUNTS)),
            directoryMounts = pairs(bundle.getStringArrayList(KEY_DIRECTORY_MOUNTS)),
            layerDirectory = bundle.getString(KEY_LAYER_DIRECTORY).orEmpty(),
            workingDirectory = bundle.getString(KEY_WORKING_DIRECTORY).orEmpty(),
            backend = bundle.getString(KEY_BACKEND).orEmpty(),
            liveNetwork = bundle.getBoolean(KEY_LIVE_NETWORK),
            maxDisplayMode = bundle.getString(KEY_MAX_DISPLAY_MODE).orEmpty(),
            snapshotDirectory = bundle.getString(KEY_SNAPSHOT_DIRECTORY).orEmpty(),
            resumeFile = bundle.getString(KEY_RESUME_FILE).orEmpty(),
            registryFiles = bundle.getStringArrayList(KEY_REGISTRY_FILES).orEmpty(),
            expandedRegistryFiles = bundle.getStringArrayList(KEY_EXPANDED_REGISTRY_FILES).orEmpty(),
            registryState = bundle.getString(KEY_REGISTRY_STATE).orEmpty(),
            managedArguments = bundle.getStringArrayList(KEY_MANAGED_ARGUMENTS).orEmpty(),
        )

        private fun pairs(flat: List<String>?): Map<String, String> =
            flat.orEmpty().chunked(2).filter { it.size == 2 }.associate { it[0] to it[1] }
    }
}
