package dev.sogen.app

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Message
import android.webkit.CookieManager
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebStorage
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout

/**
 * Shows a store's own login page and returns what it ends on. The page runs in the WebView alone: Sogen never sees
 * what is typed into it.
 */
abstract class StoreSignInActivity : Activity() {
    protected lateinit var webView: WebView
        private set
    private var popup: WebView? = null
    private var finished = false

    /** Whether the sign-in may start at [url]. */
    protected abstract fun isLoginPage(url: Uri): Boolean

    /** Whether a navigation of the main frame to [url] ends the sign-in; the subclass then calls [complete]. */
    protected open fun endsSignIn(view: WebView, url: Uri): Boolean = false

    /** Called once a page of the main frame has loaded. */
    protected open fun onPageLoaded(view: WebView, url: Uri) {}

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val start = intent.getStringExtra(EXTRA_URL)?.let(Uri::parse)
        if (start == null || start.scheme != "https" || !isLoginPage(start)) {
            setResult(RESULT_CANCELED)
            finish()
            return
        }

        val layout = FrameLayout(this)
        webView = createWebView()
        layout.addView(webView)
        setContentView(layout)

        // A restored activity starts over, so each sign-in redeems at most one code.
        webView.loadUrl(start.toString())
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun createWebView(): WebView = WebView(this).apply {
        with(settings) {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = false
            allowContentAccess = false
            mixedContentMode = WebSettings.MIXED_CONTENT_NEVER_ALLOW
            javaScriptCanOpenWindowsAutomatically = true
            setSupportMultipleWindows(true)
        }
        webViewClient = navigationGuard
        webChromeClient = popups
    }

    private val navigationGuard: WebViewClient = object : WebViewClient() {
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean {
            if (!request.isForMainFrame) {
                return false
            }

            val url = request.url
            if (endsSignIn(view, url)) {
                return true
            }

            return url.scheme != "https"
        }

        override fun onPageFinished(view: WebView, url: String) {
            if (view == webView && !finished) {
                onPageLoaded(view, Uri.parse(url))
            }
        }
    }

    /**
     * Signing in with another account, like Google, happens in a popup that reports back to the login page through
     * its opener, so the popup needs a window of its own.
     */
    private val popups: WebChromeClient = object : WebChromeClient() {
        override fun onCreateWindow(view: WebView, isDialog: Boolean, isUserGesture: Boolean, resultMsg: Message): Boolean {
            if (popup != null) {
                return false
            }

            val window = createWebView()
            popup = window
            (webView.parent as FrameLayout).addView(window)
            (resultMsg.obj as WebView.WebViewTransport).webView = window
            resultMsg.sendToTarget()
            return true
        }

        override fun onCloseWindow(window: WebView) {
            if (window == popup) {
                closePopup()
            }
        }
    }

    private fun closePopup() {
        val window = popup ?: return
        popup = null
        (window.parent as? FrameLayout)?.removeView(window)
        window.destroy()
    }

    @Deprecated("The activity has no other back handling")
    override fun onBackPressed() {
        when {
            popup?.canGoBack() == true -> popup?.goBack()
            popup != null -> closePopup()
            webView.canGoBack() -> webView.goBack()
            else -> @Suppress("DEPRECATION") super.onBackPressed()
        }
    }

    override fun onDestroy() {
        closePopup()
        if (::webView.isInitialized) {
            webView.stopLoading()
            webView.destroy()
        }
        super.onDestroy()
    }

    /** Ends the sign-in with [result]; null ends it as if the user left. */
    protected fun complete(result: String?) {
        if (finished) {
            return
        }

        finished = true
        webView.stopLoading()
        if (result == null) {
            setResult(RESULT_CANCELED)
        } else {
            setResult(RESULT_OK, Intent().putExtra(EXTRA_RESULT, result))
        }
        finish()
    }

    companion object {
        const val EXTRA_URL = "url"
        const val EXTRA_RESULT = "result"

        /**
         * Forgets the login the WebView kept for [origins]. The cookie store is shared with the ads, so only the
         * store's cookies are expired instead of clearing it.
         */
        fun forgetLogin(origins: List<String>, cookieDomain: String) {
            val cookies = CookieManager.getInstance()
            for (origin in origins) {
                val names = cookies.getCookie(origin)?.split(";")?.map { it.substringBefore('=').trim() }.orEmpty()
                for (name in names.filter(String::isNotEmpty)) {
                    cookies.setCookie(origin, "$name=; Max-Age=0; Path=/")
                    cookies.setCookie(origin, "$name=; Max-Age=0; Path=/; Domain=$cookieDomain")
                }
                WebStorage.getInstance().deleteOrigin(origin)
            }
            cookies.flush()
        }
    }
}
