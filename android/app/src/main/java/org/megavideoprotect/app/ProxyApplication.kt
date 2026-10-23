package org.megavideoprotect.app

/** Services may be started without MainActivity after process recreation. */
class ProxyApplication : android.app.Application() {
    override fun onCreate() {
        super.onCreate()
        AppProxy.initialize(this)
    }
}
