// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import android.Manifest
import android.app.Activity
import android.app.Application
import android.app.Dialog
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.util.Base64
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.bottomsheet.BottomSheetDialog
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.snackbar.Snackbar
import java.io.File
import java.lang.ref.WeakReference
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.citron.citron_emu.R
import org.citron.citron_emu.CitronApplication

/**
 * The OpenPak moments that are not a page of the OpenPak screen, as emulators/prds/openpak-ux-spec.md
 * has them on Android: which profile plays at startup (3.4), the first-run setup (3.2), the
 * full-screen sign-in (3.3), the sign-out confirmation (3.5), the friend picker (3.7), the
 * invitation prompt (3.8), the conflict sheet (3.9), and snackbars and notifications (4.3).
 */
object OpenPakUi : Application.ActivityLifecycleCallbacks {
    private var current: WeakReference<FragmentActivity>? = null
    private var liveActivities = 0

    /** The activity on screen, for a dialog the network asked for. */
    val currentActivity: FragmentActivity?
        get() = current?.get()?.takeIf { !it.isFinishing && !it.isDestroyed }

    /** Opens the OpenPak screen at a page; the app's home screen sets it. */
    var openScreen: ((FragmentActivity, Int) -> Unit)? = null

    const val STARTUP_ASK = "ask"
    const val EXTRA_PAGE = "openpak_page"
    const val PAGE_FRIENDS = 1
    const val PAGE_INVITATIONS = 2
    const val PAGE_SAVES = 3
    private const val CHANNEL = "openpak"
    private const val SNACKBAR_MILLIS = 6000

    fun install(application: Application) {
        application.registerActivityLifecycleCallbacks(this)
        val manager = application.getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL,
                application.getString(R.string.openpak_android_channel),
                NotificationManager.IMPORTANCE_DEFAULT
            ).apply { description = application.getString(R.string.openpak_android_channel_description) }
        )
        MainScope().launch { OpenPak.events.collect { show(it) } }
    }

    private fun context(): Context = CitronApplication.appContext

    private fun string(id: Int, vararg args: Any): String = context().getString(id, *args)

    /** The locale's short date and time: the one time format OpenPak uses (spec 5.4). */
    fun time(unixSeconds: Long): String =
        if (unixSeconds <= 0) {
            string(R.string.openpak_common_none)
        } else {
            DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
                .format(Date(unixSeconds * 1000))
        }

    fun time(rfc3339: String): String =
        runCatching { time(java.time.Instant.parse(rfc3339).epochSecond) }.getOrDefault(rfc3339)

    /** A §3.10 event while the app is in front: a snackbar for six seconds. */
    fun snackbar(text: String) {
        val activity = currentActivity
        val root = activity?.findViewById<View>(android.R.id.content)
        if (root == null) {
            Toast.makeText(context(), text, Toast.LENGTH_LONG).show()
            return
        }
        Snackbar.make(root, text, SNACKBAR_MILLIS).show()
    }

    /** A game invitation or a friend request: a system notification on the OpenPak channel. */
    private fun notify(id: Int, title: String, text: String, page: Int) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context(), Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            snackbar(text)
            return
        }
        val launch = context().packageManager.getLaunchIntentForPackage(context().packageName)
            ?.apply {
                putExtra(EXTRA_PAGE, page)
                addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP)
            } ?: return
        val pending = PendingIntent.getActivity(
            context(),
            page,
            launch,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val notification = NotificationCompat.Builder(context(), CHANNEL)
            .setSmallIcon(R.drawable.ic_openpak)
            .setContentTitle(title)
            .setContentText(text)
            .setContentIntent(pending)
            .setAutoCancel(true)
            .build()
        runCatching { NotificationManagerCompat.from(context()).notify(id, notification) }
    }

    private fun show(event: OpenPak.Event) {
        val on = OpenPak.notificationsEnabled
        when (event) {
            is OpenPak.Event.Message -> snackbar(event.text)
            is OpenPak.Event.FriendOnline -> if (on) {
                snackbar(
                    if (event.game.isEmpty()) {
                        string(R.string.openpak_toast_friend_online, event.name)
                    } else {
                        string(R.string.openpak_toast_friend_playing, event.name, event.game)
                    }
                )
            }
            is OpenPak.Event.FriendRequest -> if (on) {
                notify(
                    event.name.hashCode(),
                    string(R.string.openpak_toast_cat_friend_request),
                    string(R.string.openpak_toast_friend_request, event.name),
                    PAGE_FRIENDS
                )
            }
            is OpenPak.Event.Invitation -> if (on) {
                notify(
                    (event.name + event.game).hashCode(),
                    string(R.string.openpak_toast_cat_invite),
                    string(R.string.openpak_toast_invitation, event.name, event.game),
                    PAGE_INVITATIONS
                )
            }
            is OpenPak.Event.InvitationOffer -> offerInvitation(event)
            is OpenPak.Event.PickFriends -> pickFriends(event)
            is OpenPak.Event.SavesPulled -> if (on) snackbar(string(R.string.openpak_toast_saves_pulled, event.game))
            is OpenPak.Event.SavesPushed -> if (on) snackbar(string(R.string.openpak_toast_saves_pushed, event.game))
            is OpenPak.Event.SavesPushFailed ->
                snackbar(string(R.string.openpak_toast_saves_push_failed, event.game, event.error))
            is OpenPak.Event.SavesConflict -> snackbar(string(R.string.openpak_toast_saves_conflict, event.game))
            is OpenPak.Event.RedirectsChanged ->
                if (on) snackbar(string(R.string.openpak_toast_redirects_changed_emulator))
        }
    }

    // ---- the invitation prompt (3.8) ----

    private fun offerInvitation(event: OpenPak.Event.InvitationOffer) {
        val activity = currentActivity ?: return // it waits on the Invitations page
        val body = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        body.addView(text(activity, event.name, 18f, bold = true))
        body.addView(text(activity, activity.getString(R.string.openpak_invite_received_action)))
        body.addView(text(activity, event.game, 16f, bold = true).apply { setPadding(0, dp(activity, 12), 0, 0) })
        if (event.createdAt > 0) {
            body.addView(text(activity, activity.getString(R.string.openpak_invite_sent, time(event.createdAt)), dim = true))
        }
        if (event.message.isNotEmpty()) {
            body.addView(text(activity, "“${event.message}”").apply { setPadding(0, dp(activity, 12), 0, 0) })
        }
        val answer = { join: Boolean ->
            activity.lifecycleScope.launch {
                val error = OpenPak.action(
                    "invitation_action",
                    JSONObject().put("id", event.id).put("source", "console")
                        .put("action", if (join) "join" else "dismiss")
                )
                if (error.isNotEmpty()) snackbar(activity.getString(R.string.openpak_invite_handover_failed))
            }
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_invite_received_title)
            .setView(frame(activity, body))
            .setPositiveButton(R.string.openpak_invites_join) { _, _ -> answer(true) }
            .setNegativeButton(R.string.openpak_invites_ignore) { _, _ -> answer(false) }
            .setCancelable(false)
            .show()
    }

    // ---- the friend picker (3.7): a full-screen dialog, Send invite in the app bar ----

    private fun pickFriends(event: OpenPak.Event.PickFriends) {
        val activity = currentActivity
        if (activity == null) {
            OpenPak.friendsPicked(emptyList())
            return
        }
        var answered = false
        val finish = { ids: List<String> ->
            if (!answered) {
                answered = true
                OpenPak.friendsPicked(ids)
            }
        }
        val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val hint = when {
            event.friends.isEmpty() -> R.string.openpak_invite_no_friends
            event.max == 1 -> R.string.openpak_invite_hint_one
            else -> 0
        }
        list.addView(
            text(
                activity,
                if (hint != 0) activity.getString(hint) else activity.getString(R.string.openpak_invite_hint_many, event.max)
            ).apply { setPadding(0, 0, 0, dp(activity, 8)) }
        )
        val boxes = event.friends.map { friend ->
            val state = when (friend.state) {
                2 -> activity.getString(R.string.openpak_friends_playing_unknown)
                1 -> activity.getString(R.string.openpak_friends_online)
                else -> activity.getString(R.string.openpak_friends_offline)
            }
            CheckBox(activity).apply {
                text = "${friend.name}\n$state"
                minHeight = dp(activity, 48)
            }.also { list.addView(it) }
        }
        val screen = fullScreen(
            activity,
            activity.getString(R.string.openpak_invite_title),
            list,
            activity.getString(R.string.openpak_invite_send)
        ) { dialog ->
            finish(event.friends.filterIndexed { i, _ -> boxes[i].isChecked }.map { it.id })
            dialog.dismiss()
        }
        screen.dialog.setOnDismissListener { finish(emptyList()) }
        screen.action.isEnabled = false
        boxes.forEach { box ->
            box.setOnCheckedChangeListener { changed, checked ->
                // With a maximum of one they behave like radio buttons; once full, the rest wait.
                if (checked && event.max == 1) boxes.filter { it !== changed }.forEach { it.isChecked = false }
                val chosen = boxes.count { it.isChecked }
                if (event.max > 1) boxes.forEach { it.isEnabled = it.isChecked || chosen < event.max }
                screen.action.isEnabled = chosen > 0
            }
        }
    }

    // ---- startup: which profile, set it up once, then online ----

    /** A plain launch of the game list (3.2, 3.4). */
    fun runStartup(activity: FragmentActivity) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(activity, Manifest.permission.POST_NOTIFICATIONS) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(activity, arrayOf(Manifest.permission.POST_NOTIFICATIONS), 0)
        }
        activity.lifecycleScope.launch {
            if (!OpenPak.status().redirect) return@launch
            pickStartupProfile(activity) {
                OpenPak.start()
                activity.lifecycleScope.launch {
                    if (!OpenPak.setupOffered && !OpenPak.status().websiteSignedIn) {
                        OpenPak.setupOffered = true
                        runSetup(activity, false) {}
                    }
                }
            }
        }
    }

    private fun pickStartupProfile(activity: FragmentActivity, then: () -> Unit) {
        activity.lifecycleScope.launch {
            val profiles = OpenPak.profiles()
            val choice = OpenPak.startupProfile
            if (profiles.size < 2 || choice.isEmpty()) {
                then()
                return@launch
            }
            if (choice != STARTUP_ASK) {
                profiles.firstOrNull { it.uuid == choice && !it.current }?.let { selectProfile(it.uuid) }
                then()
                return@launch
            }
            showProfilePicker(activity, profiles, then)
        }
    }

    /** "Who is playing?": profiles with their badge line, Remember my choice, Add account. */
    fun showProfilePicker(activity: FragmentActivity, profiles: List<OpenPak.Profile>, then: () -> Unit) {
        val group = RadioGroup(activity)
        profiles.forEachIndexed { index, profile ->
            group.addView(RadioButton(activity).apply {
                id = View.generateViewId()
                tag = index
                minHeight = dp(activity, 48)
                text = "${profile.name}\n" + profile.account.ifEmpty { activity.getString(R.string.openpak_picker_offline) }
                isChecked = profile.current
            })
        }
        if (group.checkedRadioButtonId == -1 && group.childCount > 0) (group.getChildAt(0) as RadioButton).isChecked = true
        val remember = CheckBox(activity).apply { setText(R.string.openpak_picker_remember) }
        val body = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            addView(group)
            addView(remember)
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_picker_title)
            .setView(frame(activity, ScrollView(activity).apply { addView(body) }))
            .setPositiveButton(R.string.openpak_picker_continue) { _, _ ->
                val chosen = profiles[group.findViewById<RadioButton>(group.checkedRadioButtonId).tag as Int]
                if (remember.isChecked) OpenPak.startupProfile = chosen.uuid
                activity.lifecycleScope.launch {
                    if (!chosen.current) selectProfile(chosen.uuid)
                    then()
                }
            }
            .setNeutralButton(R.string.openpak_picker_add) { _, _ -> runSetup(activity, true, then) }
            .setNegativeButton(R.string.openpak_common_cancel) { _, _ -> then() }
            .setCancelable(false)
            .show()
    }

    /** Make a profile the current one; the session takes the old account offline and signs this in. */
    suspend fun selectProfile(uuid: String) {
        OpenPak.profileAction("select", uuid)
        withContext(Dispatchers.IO) { NativeConfig.saveGlobalConfig() }
    }

    /** The first-run setup (3.2), full screen: Sign in with OpenPak, Play offline, the Create an account link. */
    fun runSetup(activity: FragmentActivity, addAccount: Boolean, done: () -> Unit) {
        val body = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        body.addView(text(activity, activity.getString(R.string.openpak_setup_body)))
        val create = link(activity, activity.getString(R.string.openpak_setup_create))
        body.addView(create)
        val signIn = MaterialButton(activity).apply { setText(R.string.openpak_setup_sign_in) }
        val offline = MaterialButton(
            activity,
            null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle
        ).apply { setText(R.string.openpak_setup_offline) }
        body.addView(signIn, buttonParams(activity))
        body.addView(offline, buttonParams(activity))
        var chosen = false
        val screen = fullScreen(
            activity,
            activity.getString(if (addAccount) R.string.openpak_setup_title_add else R.string.openpak_setup_title),
            body,
            null,
            closable = addAccount
        )
        screen.dialog.setOnDismissListener { if (!chosen) done() }
        signIn.setOnClickListener {
            chosen = true
            screen.dialog.dismiss()
            setupSignIn(activity, addAccount, null, done)
        }
        create.setOnClickListener {
            chosen = true
            screen.dialog.dismiss()
            activity.lifecycleScope.launch {
                openSite(activity, OpenPak.status().site + "/register")
                setupSignIn(activity, addAccount, activity.getString(R.string.openpak_setup_verify), done)
            }
        }
        offline.setOnClickListener {
            chosen = true
            screen.dialog.dismiss()
            setupOffline(activity, addAccount, done)
        }
        // Back on first run is Play offline; in Add mode it is Cancel.
        screen.dialog.setOnCancelListener {
            if (!addAccount) {
                chosen = true
                setupOffline(activity, false, done)
            }
        }
    }

    private fun setupOffline(activity: FragmentActivity, addAccount: Boolean, done: () -> Unit) {
        activity.lifecycleScope.launch {
            val suggested = if (addAccount) "" else OpenPak.status().profileName
            val name = EditText(activity).apply {
                setText(suggested)
                inputType = InputType.TYPE_CLASS_TEXT
                setSelectAllOnFocus(true)
            }
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.openpak_setup_profile_name)
                .setView(frame(activity, name))
                .setPositiveButton(R.string.openpak_setup_offline) { _, _ ->
                    val chosen = name.text.toString().trim().take(32)
                    if (chosen.isEmpty()) {
                        runSetup(activity, addAccount, done)
                        return@setPositiveButton
                    }
                    activity.lifecycleScope.launch {
                        if (addAccount) {
                            val created = OpenPak.profileAction("create", name = chosen)
                            if (created.optString("error").isEmpty()) {
                                selectProfile(created.optString("uuid"))
                            } else {
                                snackbar(created.optString("error"))
                            }
                        } else {
                            OpenPak.currentProfile()?.let { OpenPak.profileAction("rename", it.uuid, chosen) }
                        }
                        done()
                    }
                }
                .setNegativeButton(R.string.openpak_common_cancel) { _, _ -> runSetup(activity, addAccount, done) }
                .setCancelable(false)
                .show()
        }
    }

    /** The account is kept under the current profile, so a new one is made current first and taken away again if nobody signs in. */
    private fun setupSignIn(activity: FragmentActivity, addAccount: Boolean, intro: String?, done: () -> Unit) {
        activity.lifecycleScope.launch {
            val previous = OpenPak.currentProfile()?.uuid
            var created: String? = null
            if (addAccount) {
                val made = OpenPak.profileAction("create", name = "OpenPak")
                if (made.optString("error").isNotEmpty()) {
                    snackbar(made.optString("error"))
                    done()
                    return@launch
                }
                created = made.optString("uuid")
                selectProfile(made.optString("uuid"))
            }
            showSignIn(activity, adopt = true, intro = intro) { signedIn ->
                if (signedIn) {
                    done()
                    return@showSignIn
                }
                activity.lifecycleScope.launch {
                    created?.let {
                        if (!previous.isNullOrEmpty()) selectProfile(previous)
                        OpenPak.profileAction("remove", it)
                    }
                    runSetup(activity, addAccount, done)
                }
            }
        }
    }

    /**
     * The sign-in screen (3.3): intro, inline error, Email, Password, Device name with its hint,
     * the Create an account link; Sign in in the app bar, disabled until both fields are filled
     * and while the request runs. [adopt] copies the account's name and picture into the profile.
     */
    fun showSignIn(
        activity: FragmentActivity,
        adopt: Boolean,
        intro: String? = null,
        done: (Boolean) -> Unit = {}
    ) {
        val body = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        intro?.let { body.addView(text(activity, it)) }
        val error = text(activity, "").apply {
            setTextColor(0xFFD32F2F.toInt())
            visibility = View.GONE
        }
        body.addView(error)
        val email = EditText(activity).apply {
            hint = activity.getString(R.string.openpak_signin_email)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
        }
        val password = EditText(activity).apply {
            hint = activity.getString(R.string.openpak_signin_password)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        val appName = activity.applicationInfo.loadLabel(activity.packageManager).toString()
        val device = EditText(activity).apply {
            hint = activity.getString(R.string.openpak_signin_device)
            inputType = InputType.TYPE_CLASS_TEXT
            setText(
                activity.getString(R.string.openpak_signin_device_default_single)
                    .replace("{emulator}", appName).replace("{machine}", Build.MODEL)
            )
        }
        val busy = LinearProgressIndicator(activity).apply {
            isIndeterminate = true
            visibility = View.GONE
        }
        body.addView(busy)
        body.addView(email)
        body.addView(password)
        body.addView(device)
        body.addView(text(activity, activity.getString(R.string.openpak_signin_device_hint), dim = true))
        val create = link(activity, activity.getString(R.string.openpak_setup_create))
        body.addView(create)

        var finished = false
        lateinit var screen: FullScreen
        screen = fullScreen(
            activity,
            activity.getString(R.string.openpak_signin_title),
            body,
            activity.getString(R.string.openpak_signin_submit)
        ) { _ ->
            val address = email.text.toString().trim()
            screen.action.isEnabled = false
            busy.visibility = View.VISIBLE
            error.visibility = View.GONE
            activity.lifecycleScope.launch {
                OpenPak.call("set_device_name", JSONObject().put("name", device.text.toString().trim()))
                val result = OpenPak.signIn(address, password.text.toString(), adopt)
                busy.visibility = View.GONE
                screen.action.isEnabled = true
                val failure = result.optString("error")
                if (failure.isNotEmpty()) {
                    // Inline, the dialog stays open with the email kept.
                    error.text = failure
                    error.visibility = View.VISIBLE
                    return@launch
                }
                val username = result.optString("username")
                val linkError = result.optString("link_error")
                snackbar(
                    if (linkError.isEmpty()) {
                        activity.getString(R.string.openpak_toast_signed_in_linked, username)
                    } else {
                        activity.getString(R.string.openpak_android_link_failed_detail, username, linkError)
                    }
                )
                if (adopt) adoptAccount(activity, username, result.optString("avatar"))
                finished = true
                screen.dialog.dismiss()
                done(true)
            }
        }
        screen.dialog.setOnDismissListener { if (!finished) done(false) }
        create.setOnClickListener {
            activity.lifecycleScope.launch { openSite(activity, OpenPak.status().site + "/register") }
        }
        val update = {
            screen.action.isEnabled = email.text.isNotBlank() && password.text.isNotEmpty()
        }
        listOf(email, password).forEach {
            it.addTextChangedListener(object : android.text.TextWatcher {
                override fun afterTextChanged(s: android.text.Editable?) = update()
                override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            })
        }
        update()
        email.requestFocus()
    }

    /** The sign-out confirmation (3.5): Cancel is the default; Sign out revokes the token. */
    fun confirmSignOut(activity: FragmentActivity, profileName: String, done: () -> Unit) {
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_signout_title)
            .setMessage(activity.getString(R.string.openpak_signout_body, profileName))
            .setPositiveButton(R.string.openpak_signout_confirm) { _, _ ->
                activity.lifecycleScope.launch {
                    OpenPak.signOut()
                    snackbar(activity.getString(R.string.openpak_toast_signed_out))
                    done()
                }
            }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    /** The conflict (3.9), as a bottom sheet: Keep this machine's, Take the cloud's, Decide later. */
    fun showConflict(
        activity: FragmentActivity,
        game: String,
        local: String,
        cloud: String,
        cloudAllowed: Boolean,
        choose: (keepLocal: Boolean) -> Unit
    ) {
        val sheet = BottomSheetDialog(activity)
        val body = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            val pad = dp(activity, 24)
            setPadding(pad, pad, pad, pad)
        }
        body.addView(text(activity, activity.getString(R.string.openpak_conflict_title), 20f, bold = true))
        body.addView(text(activity, activity.getString(R.string.openpak_conflict_body, game)))
        body.addView(text(activity, activity.getString(R.string.openpak_conflict_local), 16f, bold = true))
        body.addView(text(activity, local, dim = true))
        body.addView(text(activity, activity.getString(R.string.openpak_conflict_cloud), 16f, bold = true))
        body.addView(text(activity, cloud, dim = true))
        val keep = MaterialButton(activity).apply { setText(R.string.openpak_conflict_keep_local) }
        val take = MaterialButton(activity).apply {
            setText(R.string.openpak_conflict_take_cloud)
            isEnabled = cloudAllowed
        }
        val later = MaterialButton(
            activity,
            null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle
        ).apply { setText(R.string.openpak_conflict_later) }
        listOf(keep, take, later).forEach { body.addView(it, buttonParams(activity)) }
        keep.setOnClickListener {
            sheet.dismiss()
            choose(true)
        }
        take.setOnClickListener {
            sheet.dismiss()
            choose(false)
        }
        later.setOnClickListener { sheet.dismiss() }
        sheet.setContentView(ScrollView(activity).apply { addView(body) })
        sheet.show()
    }

    /** The account's name and picture, copied into the current profile once. */
    private suspend fun adoptAccount(activity: Activity, username: String, avatar: String) {
        val uuid = OpenPak.currentProfile()?.uuid ?: return
        if (username.isNotEmpty()) {
            OpenPak.profileAction("rename", uuid, username.take(32))
        }
        if (avatar.isEmpty()) return
        val file = withContext(Dispatchers.IO) {
            runCatching {
                val bytes = Base64.decode(avatar, Base64.DEFAULT)
                val source = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return@runCatching null
                val side = minOf(source.width, source.height)
                val square = Bitmap.createBitmap(
                    source,
                    (source.width - side) / 2,
                    (source.height - side) / 2,
                    side,
                    side
                )
                val scaled = Bitmap.createScaledBitmap(square, 256, 256, true)
                File(activity.cacheDir, "openpak-avatar.jpg").also { out ->
                    out.outputStream().use { scaled.compress(Bitmap.CompressFormat.JPEG, 90, it) }
                }
            }.getOrNull()
        } ?: return
        OpenPak.profileAction("image", uuid, path = file.absolutePath)
        file.delete()
    }

    // ---- the view kit every OpenPak screen shares ----

    class FullScreen(val dialog: Dialog, val action: View)

    /**
     * A full-screen dialog in the app's own theme: an app bar with close, the title and an
     * optional action, over a scrolling body.
     */
    fun fullScreen(
        activity: FragmentActivity,
        title: String,
        body: View,
        actionLabel: String?,
        closable: Boolean = true,
        onAction: (Dialog) -> Unit = {}
    ): FullScreen {
        val dialog = Dialog(activity, com.google.android.material.R.style.ThemeOverlay_Material3)
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(
                MaterialColors.getColor(activity, com.google.android.material.R.attr.colorSurface, 0)
            )
            fitsSystemWindows = true
        }
        val toolbar = MaterialToolbar(activity).apply {
            this.title = title
            if (closable) {
                setNavigationIcon(R.drawable.ic_clear)
                setNavigationOnClickListener { dialog.cancel() }
            }
        }
        val action = MaterialButton(
            activity,
            null,
            androidx.appcompat.R.attr.borderlessButtonStyle
        ).apply {
            text = actionLabel ?: ""
            visibility = if (actionLabel == null) View.GONE else View.VISIBLE
            setOnClickListener { onAction(dialog) }
        }
        toolbar.addView(
            action,
            androidx.appcompat.widget.Toolbar.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.END
            )
        )
        val pad = dp(activity, 24)
        val scroll = ScrollView(activity).apply {
            addView(body)
            setPadding(pad, pad / 2, pad, pad)
        }
        root.addView(toolbar)
        root.addView(scroll, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        dialog.setContentView(root)
        dialog.setCancelable(closable)
        dialog.window?.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        dialog.show()
        return FullScreen(dialog, action)
    }

    fun dp(context: Context, value: Int) = (value * context.resources.displayMetrics.density).toInt()

    fun text(context: Context, value: String, size: Float = 14f, bold: Boolean = false, dim: Boolean = false) =
        TextView(context).apply {
            text = value
            textSize = size
            if (bold) setTypeface(typeface, Typeface.BOLD)
            if (dim) alpha = 0.7f
            setPadding(0, dp(context, 4), 0, dp(context, 4))
        }

    fun link(context: Context, value: String) = TextView(context).apply {
        text = value
        textSize = 14f
        setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
        minHeight = dp(context, 48)
        gravity = Gravity.CENTER_VERTICAL
        isClickable = true
        isFocusable = true
    }

    private fun buttonParams(context: Context) = LinearLayout.LayoutParams(
        LinearLayout.LayoutParams.MATCH_PARENT,
        LinearLayout.LayoutParams.WRAP_CONTENT
    ).apply { topMargin = dp(context, 8) }

    fun frame(activity: Activity, view: View): View {
        val padding = dp(activity, 24)
        return LinearLayout(activity).apply {
            setPadding(padding, padding / 2, padding, 0)
            addView(
                view,
                LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            )
        }
    }

    fun openSite(activity: Activity, url: String) {
        runCatching { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
    }

    // ---- which activity is on screen, and the goodbye when the last one goes ----

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        liveActivities++
    }

    override fun onActivityResumed(activity: Activity) {
        if (activity is FragmentActivity) {
            current = WeakReference(activity)
            // A notification's tap: open the page it was about.
            val page = activity.intent?.getIntExtra(EXTRA_PAGE, -1) ?: -1
            if (page >= 0) {
                activity.intent.removeExtra(EXTRA_PAGE)
                openScreen?.invoke(activity, page)
            }
        }
    }

    override fun onActivityPaused(activity: Activity) {
        if (current?.get() === activity) current = null
    }

    override fun onActivityDestroyed(activity: Activity) {
        liveActivities--
        // The app is closing, not rotating: friends should see us leave now.
        if (liveActivities <= 0 && activity.isFinishing && !activity.isChangingConfigurations) {
            OpenPak.goOffline()
        }
    }

    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}
}
