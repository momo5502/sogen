package dev.sogen.app

import android.net.Uri
import android.webkit.WebView

/** GOG's login page, which ends by redirecting to a page whose address carries the authorization code. */
class GogSignInActivity : StoreSignInActivity() {
    override fun isLoginPage(url: Uri) = url.host == AUTH_HOST

    override fun endsSignIn(view: WebView, url: Uri): Boolean {
        if (!isRedirect(url)) {
            return false
        }
        complete(url.toString())
        return true
    }

    companion object {
        private const val AUTH_HOST = "auth.gog.com"
        private const val REDIRECT_HOST = "embed.gog.com"
        private const val REDIRECT_PATH = "/on_login_success"

        private val gogOrigins = listOf(
            "https://gog.com",
            "https://www.gog.com",
            "https://auth.gog.com",
            "https://login.gog.com",
            "https://embed.gog.com",
        )

        fun isRedirect(url: Uri) = url.scheme == "https" && url.host == REDIRECT_HOST && url.port == -1 &&
            url.path == REDIRECT_PATH

        fun forgetLogin() = forgetLogin(gogOrigins, ".gog.com")
    }
}
