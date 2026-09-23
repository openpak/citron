// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.content.Intent
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.text.format.DateUtils
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.tabs.TabLayout
import com.google.android.material.appbar.MaterialToolbar
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject
import org.citron.citron_emu.R
import org.citron.citron_emu.model.GamesViewModel
import org.citron.citron_emu.utils.OpenPak
import org.citron.citron_emu.utils.OpenPakUi
import java.time.Instant

/**
 * The OpenPak screen: the desktop account window's seven sections, in its order -- Account,
 * Friends, Invitations, Cloud saves, Mods, News, Status -- over the same website calls
 * (openpak-client's api.h), drawn as plain Android rows.
 */
class OpenPakFragment : DialogFragment() {
    private val gamesViewModel: GamesViewModel by activityViewModels()

    private lateinit var content: LinearLayout
    private lateinit var tabs: TabLayout
    private var section = 0

    /** The game the Mods and News sections look at: 16 hex digits and a name. */
    private var chosenTitle: Pair<String, String>? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setStyle(STYLE_NO_TITLE, 0)
        section = savedInstanceState?.getInt(KEY_SECTION) ?: 0
    }

    override fun onStart() {
        super.onStart()
        dialog?.window?.setLayout(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(KEY_SECTION, section)
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        val context = requireActivity()
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(
                MaterialColors.getColor(context, com.google.android.material.R.attr.colorSurface, 0)
            )
        }
        val toolbar = MaterialToolbar(context).apply {
            setTitle(R.string.openpak_title)
            setNavigationIcon(R.drawable.ic_back)
            setNavigationOnClickListener { dismiss() }
            menu.add(R.string.openpak_refresh).setIcon(R.drawable.ic_refresh)
                .setShowAsAction(android.view.MenuItem.SHOW_AS_ACTION_ALWAYS)
            setOnMenuItemClickListener {
                showSection(section)
                true
            }
        }
        tabs = TabLayout(context).apply {
            tabMode = TabLayout.MODE_SCROLLABLE
            SECTIONS.forEach { addTab(newTab().setText(it)) }
        }
        content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            val padding = dp(16)
            setPadding(padding, padding / 2, padding, padding)
        }
        val scroll = ScrollView(context).apply { addView(content) }
        root.addView(toolbar)
        root.addView(tabs)
        root.addView(
            scroll,
            LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f)
        )
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            view.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = bars.bottom)
            insets
        }
        return root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        tabs.getTabAt(section)?.select()
        tabs.addOnTabSelectedListener(object : TabLayout.OnTabSelectedListener {
            override fun onTabSelected(tab: TabLayout.Tab) = showSection(tab.position)
            override fun onTabUnselected(tab: TabLayout.Tab) {}
            override fun onTabReselected(tab: TabLayout.Tab) = showSection(tab.position)
        })
        showSection(section)
    }

    private fun showSection(index: Int) {
        section = index
        content.removeAllViews()
        addText(getString(R.string.openpak_loading), dim = true)
        viewLifecycleOwner.lifecycleScope.launch {
            val failure = runCatching {
                when (index) {
                    0 -> showAccount()
                    1 -> showFriends()
                    2 -> showInvitations()
                    3 -> showCloudSaves()
                    4 -> showMods()
                    5 -> showNews()
                    else -> showStatus()
                }
            }.exceptionOrNull()
            if (failure != null && section == index && isAdded) {
                content.removeAllViews()
                addText(getString(R.string.openpak_could_not_load, failure.message ?: ""))
            }
        }
    }

    // ---- Account ----

    private suspend fun showAccount() {
        val status = OpenPak.status()
        val profiles = OpenPak.profiles()
        if (section != 0 || !isAdded) return
        content.removeAllViews()

        addHeader(getString(R.string.openpak_profile, status.profileName))
        if (!status.redirect) {
            addText(getString(R.string.openpak_no_server), dim = true)
        }
        if (status.websiteSignedIn) {
            addText(getString(R.string.openpak_linked_as, status.username, status.friendCode))
            addText(
                getString(
                    if (status.linked) R.string.openpak_console_linked else R.string.openpak_console_not_linked
                ),
                dim = true
            )
            addButtons(
                getString(R.string.openpak_sign_out) to {
                    confirm(getString(R.string.openpak_sign_out_confirm)) {
                        lifecycleScope.launch {
                            OpenPak.signOut()
                            toast(getString(R.string.openpak_signed_out))
                            showSection(0)
                        }
                    }
                }
            )
        } else {
            addText(getString(R.string.openpak_not_linked))
            addButtons(
                getString(R.string.openpak_sign_in) to {
                    OpenPakUi.showSignIn(requireActivity(), adopt = false) { if (it) showSection(0) }
                },
                getString(R.string.openpak_create_account) to { openSite(status.site + "/register") }
            )
        }

        addHeader(getString(R.string.openpak_profiles))
        profiles.forEach { profile ->
            addRow(
                profile.name + if (profile.current) "  ✓" else "",
                if (profile.account.isEmpty()) {
                    getString(R.string.openpak_offline)
                } else {
                    getString(R.string.openpak_badge, profile.account)
                }
            ) {
                // Not while a title runs: the game holds the profile it started with.
                if (status.running.isNotEmpty()) {
                    toast(getString(R.string.openpak_stop_game_first))
                    return@addRow
                }
                val actions = mutableListOf<Pair<String, () -> Unit>>()
                if (!profile.current) {
                    actions += getString(R.string.openpak_use_profile) to {
                        lifecycleScope.launch {
                            OpenPakUi.selectProfile(profile.uuid)
                            showSection(0)
                        }
                    }
                }
                actions += getString(R.string.openpak_rename_profile) to { renameProfile(profile) }
                if (profiles.size > 1) {
                    actions += getString(R.string.openpak_delete_profile) to {
                        confirm(getString(R.string.openpak_delete_profile_confirm, profile.name)) {
                            profileAction("remove", profile.uuid)
                        }
                    }
                }
                chooseAction(profile.name, actions)
            }
        }
        addButtons(
            getString(R.string.openpak_add_account) to {
                OpenPakUi.runSetup(requireActivity(), true) { showSection(0) }
            },
            getString(R.string.openpak_account_at_startup) to { chooseStartupProfile(profiles) }
        )

        addHeader(getString(R.string.openpak_preferences))
        addSwitch(getString(R.string.openpak_notifications), OpenPak.notificationsEnabled) {
            OpenPak.notificationsEnabled = it
        }
        addSwitch(getString(R.string.openpak_cloud_sync), OpenPak.cloudSyncEnabled) {
            OpenPak.cloudSyncEnabled = it
        }
        addText(getString(R.string.openpak_server_line, status.site, status.server), dim = true)
    }

    private fun renameProfile(profile: OpenPak.Profile) {
        val name = EditText(requireActivity()).apply {
            setText(profile.name)
            inputType = InputType.TYPE_CLASS_TEXT
        }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_profile_name)
            .setView(OpenPakUi.frame(requireActivity(), name))
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val chosen = name.text.toString().trim().take(32)
                if (chosen.isNotEmpty()) profileAction("rename", profile.uuid, chosen)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun profileAction(action: String, uuid: String, name: String = "") {
        lifecycleScope.launch {
            val error = OpenPak.profileAction(action, uuid, name).optString("error")
            if (error.isNotEmpty()) toast(error)
            org.citron.citron_emu.utils.NativeConfig.saveGlobalConfig()
            if (isAdded) showSection(0)
        }
    }

    private fun chooseStartupProfile(profiles: List<OpenPak.Profile>) {
        val values = listOf("", OpenPakUi.STARTUP_ASK) + profiles.map { it.uuid }
        val labels = listOf(
            getString(R.string.openpak_last_used),
            getString(R.string.openpak_ask_every_time)
        ) + profiles.map { it.name }
        val checked = values.indexOf(OpenPak.startupProfile).coerceAtLeast(0)
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_account_at_startup)
            .setSingleChoiceItems(labels.toTypedArray(), checked) { dialog, which ->
                OpenPak.startupProfile = values[which]
                dialog.dismiss()
            }
            .show()
    }

    // ---- Friends ----

    private suspend fun showFriends() {
        val list = OpenPak.callObject("friends")
        if (section != 1 || !isAdded) return
        content.removeAllViews()
        addButtons(getString(R.string.openpak_add_friend) to { addFriend() })
        if (!list.optBoolean("ok")) {
            addText(list.optString("error").ifEmpty { getString(R.string.openpak_sign_in_first) }, dim = true)
            return
        }
        val running = list.optString("running")
        val requests = list.optJSONArray("requests") ?: JSONArray()
        addHeader(getString(R.string.openpak_friend_requests))
        if (requests.length() == 0) {
            addText(getString(R.string.openpak_friends_requests_empty), dim = true)
        }
        run {
            requests.forEachObject { request ->
                val incoming = request.optBoolean("incoming")
                addRow(
                    request.optString("name"),
                    getString(if (incoming) R.string.openpak_wants_to_be_friends else R.string.openpak_request_sent)
                )
                if (incoming) {
                    addButtons(
                        getString(R.string.openpak_accept) to { friendAction("accept", request) },
                        getString(R.string.openpak_decline) to { friendAction("decline", request) }
                    )
                } else {
                    addButtons(getString(R.string.openpak_cancel_request) to { friendAction("decline", request) })
                }
            }
        }
        val friends = list.optJSONArray("friends") ?: JSONArray()
        addHeader(getString(R.string.openpak_friends_list))
        if (friends.length() == 0) {
            addText(getString(R.string.openpak_friends_empty), dim = true)
        }
        friends.forEachObject { friend ->
            val game = friend.optString("game")
            val state = when {
                friend.optBoolean("online") && game.isNotEmpty() ->
                    getString(R.string.openpak_friends_playing, game)
                friend.optBoolean("online") -> getString(R.string.openpak_state_online)
                else -> getString(R.string.openpak_state_offline)
            }
            addRow(friend.optString("name"), "$state · ${friend.optString("friend_code")}") {
                val joinable = running.isNotEmpty() &&
                    friend.optString("title_id").equals(running, ignoreCase = true)
                val actions = mutableListOf<Pair<String, () -> Unit>>()
                if (joinable) actions += getString(R.string.openpak_join) to { friendAction("join", friend) }
                actions += getString(R.string.openpak_remove_friend) to {
                    confirm(getString(R.string.openpak_remove_confirm, friend.optString("name"))) {
                        friendAction("remove", friend)
                    }
                }
                actions += getString(R.string.openpak_block) to {
                    confirm(getString(R.string.openpak_block_confirm, friend.optString("name"))) {
                        friendAction("block", friend)
                    }
                }
                chooseAction(friend.optString("name"), actions)
            }
        }
    }

    private fun addFriend() {
        val code = EditText(requireActivity()).apply {
            hint = getString(R.string.openpak_friend_code)
            inputType = InputType.TYPE_CLASS_TEXT
        }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_add_friend)
            .setView(OpenPakUi.frame(requireActivity(), code))
            .setPositiveButton(R.string.openpak_send_request) { _, _ ->
                val typed = code.text.toString().trim()
                if (typed.isEmpty()) {
                    toast(getString(R.string.openpak_friends_no_code))
                } else {
                    friendAction("add", JSONObject().put("code", typed))
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun friendAction(action: String, target: JSONObject) {
        val args = JSONObject(target.toString()).put("action", action)
        lifecycleScope.launch {
            val error = OpenPak.action("friend_action", args)
            toast(
                error.ifEmpty {
                    if (action == "add") {
                        getString(R.string.openpak_friends_added, target.optString("code"))
                    } else {
                        getString(R.string.openpak_done)
                    }
                }
            )
            if (isAdded) showSection(1)
        }
    }

    // ---- Invitations ----

    private suspend fun showInvitations() {
        val list = OpenPak.callArray("invitations")
        if (section != 2 || !isAdded) return
        content.removeAllViews()
        if (list.length() == 0) {
            addText(getString(R.string.openpak_no_invitations), dim = true)
        }
        list.forEachObject { invitation ->
            val message = invitation.optString("message")
            addRow(
                invitation.optString("game"),
                getString(R.string.openpak_invitations_from, invitation.optString("from")) +
                    if (message.isEmpty()) "" else "\n$message"
            )
            val answer = { action: String ->
                lifecycleScope.launch {
                    val error = OpenPak.action(
                        "invitation_action",
                        JSONObject().put("id", invitation.optString("id"))
                            .put("source", invitation.optString("source")).put("action", action)
                    )
                    if (error.isNotEmpty()) toast(error)
                    if (isAdded) showSection(2)
                }
            }
            if (invitation.optBoolean("joinable")) {
                addButtons(
                    getString(R.string.openpak_play) to { answer("join") },
                    getString(R.string.openpak_decline) to { answer("dismiss") }
                )
            } else {
                addButtons(getString(R.string.openpak_decline) to { answer("dismiss") })
            }
        }
    }

    // ---- Cloud saves ----

    private suspend fun showCloudSaves() {
        val saves = OpenPak.callObject("cloud_saves")
        if (section != 3 || !isAdded) return
        content.removeAllViews()
        if (!saves.optBoolean("ok")) {
            addText(saves.optString("error").ifEmpty { getString(R.string.openpak_sign_in_first) }, dim = true)
            return
        }
        val allowance = saves.optLong("allowance")
        addText(
            if (allowance > 0) {
                getString(
                    R.string.openpak_allowance,
                    Formatter.formatShortFileSize(requireContext(), saves.optLong("used")),
                    Formatter.formatShortFileSize(requireContext(), allowance)
                )
            } else {
                getString(
                    R.string.openpak_allowance_used,
                    Formatter.formatShortFileSize(requireContext(), saves.optLong("used"))
                )
            },
            dim = true
        )
        val running = saves.optString("running").isNotEmpty()
        val titles = saves.optJSONArray("titles") ?: JSONArray()
        if (titles.length() == 0) {
            addText(getString(R.string.openpak_no_cloud_saves), dim = true)
        }
        titles.forEachObject { title ->
            val versions = title.optJSONArray("versions") ?: JSONArray()
            val newest = if (versions.length() > 0) versions.getJSONObject(0) else null
            val cloud = newest?.let {
                getString(
                    R.string.openpak_cloud_version,
                    it.optInt("number"),
                    relativeTime(it.optString("saved_at")),
                    it.optString("device")
                )
            } ?: ""
            addRow(title.optString("name"), "$cloud\n${localState(title.optString("local"))}") {
                showVersions(title)
            }
            if (!running) {
                val args = JSONObject().put("title_id", title.optString("title_id"))
                    .put("newest", title.optInt("newest"))
                addButtons(
                    getString(R.string.openpak_download) to {
                        confirm(getString(R.string.openpak_download_confirm, title.optString("name"))) {
                            saveAction("download", args)
                        }
                    },
                    getString(R.string.openpak_upload) to { saveAction("upload", args) }
                )
            }
        }
        if (running) addText(getString(R.string.openpak_stop_game_first), dim = true)
    }

    private fun localState(state: String): String =
        getString(
            when (state) {
                "in_step" -> R.string.openpak_local_in_step
                "changed_here" -> R.string.openpak_local_changed
                "cloud_newer" -> R.string.openpak_local_cloud_newer
                "no_history" -> R.string.openpak_local_no_history
                else -> R.string.openpak_local_none
            }
        )

    private fun showVersions(title: JSONObject) {
        val versions = title.optJSONArray("versions") ?: return
        val labels = mutableListOf<String>()
        versions.forEachObject {
            labels += getString(
                R.string.openpak_cloud_version,
                it.optInt("number"),
                relativeTime(it.optString("saved_at")),
                it.optString("device")
            ) + " · " + Formatter.formatShortFileSize(requireContext(), it.optLong("size")) +
                if (it.optBoolean("conflict")) " · " + getString(R.string.openpak_conflict) else ""
        }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(title.optString("name"))
            .setItems(labels.toTypedArray()) { _, which ->
                val version = versions.getJSONObject(which)
                confirm(getString(R.string.openpak_delete_version_confirm, version.optInt("number"))) {
                    saveAction("delete", JSONObject().put("version_id", version.optLong("id")))
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun saveAction(action: String, args: JSONObject) {
        lifecycleScope.launch {
            val error = OpenPak.action("cloud_save_action", JSONObject(args.toString()).put("action", action))
            toast(error.ifEmpty { getString(R.string.openpak_done) })
            if (isAdded) showSection(3)
        }
    }

    // ---- Mods ----

    private suspend fun showMods() {
        val title = chosenTitle ?: pickDefaultTitle()
        if (section != 4 || !isAdded) return
        content.removeAllViews()
        addTitlePicker(title)
        if (title == null) return
        val mods = OpenPak.callArray("mods", JSONObject().put("title_id", title.first))
        if (section != 4 || !isAdded) return
        if (mods.length() == 0) {
            addText(getString(R.string.openpak_no_mods), dim = true)
        }
        mods.forEachObject { mod ->
            val favourite = mod.optBoolean("favourite")
            addRow(
                (if (favourite) "★ " else "") + mod.optString("name") + " " + mod.optString("version"),
                listOf(mod.optString("author"), mod.optString("summary")).filter { it.isNotEmpty() }
                    .joinToString("\n")
            )
            val args = JSONObject().put("title_id", title.first).put("mod_id", mod.optString("id"))
            val installed = mod.optBoolean("installed")
            addButtons(
                getString(if (installed) R.string.openpak_uninstall else R.string.openpak_install) to {
                    modAction(if (installed) "uninstall" else "install", args)
                },
                getString(if (favourite) R.string.openpak_unfavourite else R.string.openpak_favourite) to {
                    modAction(if (favourite) "unfavourite" else "favourite", args)
                }
            )
        }
    }

    private fun modAction(action: String, args: JSONObject) {
        lifecycleScope.launch {
            val error = OpenPak.action("mod_action", JSONObject(args.toString()).put("action", action))
            toast(error.ifEmpty { getString(R.string.openpak_done) })
            if (isAdded) showSection(4)
        }
    }

    // ---- News ----

    private suspend fun showNews() {
        val title = chosenTitle ?: pickDefaultTitle()
        if (section != 5 || !isAdded) return
        content.removeAllViews()
        addTitlePicker(title)
        if (title == null) return
        val news = OpenPak.callObject("news", JSONObject().put("title_id", title.first))
        if (section != 5 || !isAdded) return
        val files = news.optJSONArray("files") ?: JSONArray()
        if (news.optString("valid_from").isNotEmpty()) {
            addText(getString(R.string.openpak_news_from, relativeTime(news.optString("valid_from"))), dim = true)
        }
        if (files.length() == 0) {
            addText(getString(R.string.openpak_no_news), dim = true)
        }
        files.forEachObject {
            addRow(it.optString("path"), Formatter.formatShortFileSize(requireContext(), it.optLong("size")))
        }
    }

    // ---- Status ----

    private suspend fun showStatus() {
        val status = OpenPak.callObject("network_status")
        if (section != 6 || !isAdded) return
        content.removeAllViews()
        addHeader(
            status.optString("headline").ifEmpty {
                getString(R.string.openpak_status_unreachable, status.optString("url"))
            }
        )
        if (status.optString("sub").isNotEmpty()) addText(status.optString("sub"), dim = true)
        addText(
            getString(
                if (status.optBoolean("online")) R.string.openpak_you_are_online else R.string.openpak_you_are_offline
            )
        )
        if (status.optBoolean("network_ok")) {
            addText(getString(R.string.openpak_players_online, status.optInt("players_online")))
        }
        val titles = status.optJSONArray("titles") ?: JSONArray()
        if (titles.length() > 0) {
            addHeader(getString(R.string.openpak_playing_now))
            titles.forEachObject {
                addRow(it.optString("name"), getString(R.string.openpak_players, it.optInt("players")))
            }
        }
        val services = status.optJSONArray("services") ?: JSONArray()
        if (services.length() > 0) {
            addHeader(getString(R.string.openpak_services))
            services.forEachObject {
                val state = getString(if (it.optBoolean("up")) R.string.openpak_up else R.string.openpak_down)
                addRow(
                    listOf(it.optString("group"), it.optString("name")).filter { s -> s.isNotEmpty() }
                        .joinToString(" · "),
                    "$state · ${it.optString("latency")} · " +
                        String.format("%.2f%%", it.optDouble("uptime"))
                )
            }
        }
    }

    // ---- the title picker Mods and News share: the library, else the catalogue ----

    private suspend fun titles(): List<Pair<String, String>> {
        val installed = gamesViewModel.games.value
            .filter { it.programId.isNotEmpty() && it.programId != "0" && !it.isHomebrew }
            .map { java.lang.Long.toHexString(it.programId.toLong()).padStart(16, '0').uppercase() to it.title }
            .distinctBy { it.first }
            .sortedBy { it.second.lowercase() }
        if (installed.isNotEmpty()) return installed
        val catalogue = OpenPak.callArray("catalogue")
        val out = mutableListOf<Pair<String, String>>()
        catalogue.forEachObject { out += it.optString("title_id").uppercase() to it.optString("name") }
        return out.sortedBy { it.second.lowercase() }
    }

    private suspend fun pickDefaultTitle(): Pair<String, String>? {
        chosenTitle = titles().firstOrNull()
        return chosenTitle
    }

    private fun addTitlePicker(title: Pair<String, String>?) {
        addButtons(
            (title?.second ?: getString(R.string.openpak_no_games)) + "  ▾" to {
                lifecycleScope.launch {
                    val all = titles()
                    if (all.isEmpty() || !isAdded) return@launch
                    MaterialAlertDialogBuilder(requireActivity())
                        .setTitle(R.string.openpak_choose_game)
                        .setItems(all.map { it.second }.toTypedArray()) { _, which ->
                            chosenTitle = all[which]
                            showSection(section)
                        }
                        .show()
                }
            }
        )
    }

    // ---- row kit ----

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private fun addHeader(text: String) {
        content.addView(TextView(requireActivity()).apply {
            this.text = text
            textSize = 18f
            setTypeface(typeface, Typeface.BOLD)
            setPadding(0, dp(16), 0, dp(4))
        })
    }

    private fun addText(text: String, dim: Boolean = false) {
        content.addView(TextView(requireActivity()).apply {
            this.text = text
            textSize = 14f
            if (dim) alpha = 0.7f
            setPadding(0, dp(4), 0, dp(4))
        })
    }

    private fun addRow(title: String, detail: String, onClick: (() -> Unit)? = null) {
        val row = LinearLayout(requireActivity()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(8), 0, dp(8))
            if (onClick != null) {
                isClickable = true
                isFocusable = true
                val outValue = android.util.TypedValue()
                context.theme.resolveAttribute(android.R.attr.selectableItemBackground, outValue, true)
                setBackgroundResource(outValue.resourceId)
                setOnClickListener { onClick() }
            }
        }
        row.addView(TextView(requireActivity()).apply {
            text = title
            textSize = 16f
        })
        if (detail.isNotEmpty()) {
            row.addView(TextView(requireActivity()).apply {
                text = detail
                textSize = 13f
                alpha = 0.7f
            })
        }
        content.addView(row)
    }

    private fun addButtons(vararg buttons: Pair<String, () -> Unit>) {
        val bar = LinearLayout(requireActivity()).apply { orientation = LinearLayout.HORIZONTAL }
        buttons.forEach { (label, action) ->
            bar.addView(
                MaterialButton(
                    requireActivity(),
                    null,
                    com.google.android.material.R.attr.materialButtonOutlinedStyle
                ).apply {
                    text = label
                    isAllCaps = false
                    setOnClickListener { action() }
                },
                LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.WRAP_CONTENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                ).apply { marginEnd = dp(8) }
            )
        }
        content.addView(bar)
    }

    private fun addSwitch(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
        content.addView(MaterialSwitch(requireActivity()).apply {
            text = label
            isChecked = checked
            setOnCheckedChangeListener { _, value -> onChange(value) }
        })
    }

    private fun chooseAction(title: String, actions: List<Pair<String, () -> Unit>>) {
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(title)
            .setItems(actions.map { it.first }.toTypedArray()) { _, which -> actions[which].second() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirm(message: String, action: () -> Unit) {
        MaterialAlertDialogBuilder(requireActivity())
            .setMessage(message)
            .setPositiveButton(android.R.string.ok) { _, _ -> action() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun toast(text: String) {
        Toast.makeText(requireContext().applicationContext, text, Toast.LENGTH_LONG).show()
    }

    private fun openSite(url: String) {
        runCatching { startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))) }
    }

    private fun relativeTime(rfc3339: String): String =
        runCatching {
            DateUtils.getRelativeTimeSpanString(Instant.parse(rfc3339).toEpochMilli()).toString()
        }.getOrDefault(rfc3339)

    private inline fun JSONArray.forEachObject(action: (JSONObject) -> Unit) {
        for (index in 0 until length()) action(getJSONObject(index))
    }

    companion object {
        const val TAG = "OpenPakFragment"
        private const val KEY_SECTION = "section"

        private val SECTIONS = listOf(
            R.string.openpak_section_account,
            R.string.openpak_section_friends,
            R.string.openpak_section_invitations,
            R.string.openpak_section_cloud_saves,
            R.string.openpak_section_mods,
            R.string.openpak_section_news,
            R.string.openpak_section_status
        )
    }
}
