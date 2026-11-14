package me.masonasons.fastplay.ui

import android.text.InputType
import androidx.annotation.DrawableRes
import me.masonasons.fastplay.R
import me.masonasons.fastplay.engine.Engine
import me.masonasons.fastplay.files.AutoSyncScreen
import me.masonasons.fastplay.files.FolderResume
import java.io.File

/** One row of a settings screen. */
sealed class SettingRow {
    class Toggle(val title: String, val isOn: () -> Boolean, val set: (Boolean) -> Unit) : SettingRow()
    /** One of a few named values. */
    class Choice(val title: String, val options: List<String>, val selected: () -> Int, val set: (Int) -> Unit) : SettingRow()
    /** A number typed in, kept within `range`. */
    class Number(
        val title: String, val unit: String, val range: ClosedFloatingPointRange<Double>,
        val value: () -> Double, val set: (Double) -> Unit,
    ) : SettingRow()
    /** A file chosen in the system's picker (and Clear, once one is). */
    class FileChoice(val title: String, val mimeTypes: Array<String>, val name: () -> String, val set: (String?) -> Boolean) : SettingRow()
    class Info(val title: String, val detail: String) : SettingRow()
    /** Opens another screen. */
    class Opens(val title: String, val detail: String, @DrawableRes val icon: Int, val make: () -> Screen) : SettingRow()
}

class SettingSection(val title: String?, val footer: String?, val rows: List<SettingRow>)

/** A screen of settings: switches, choices, numbers and files, in sections. */
open class SettingsPageScreen() : ListScreen() {
    private var pageTitle = "Settings"
    private var makeSections: () -> List<SettingSection> = { emptyList() }

    constructor(title: String, sections: () -> List<SettingSection>) : this() {
        pageTitle = title
        makeSections = sections
    }

    override val title get() = pageTitle

    private fun text(value: Double) =
        if (value == Math.rint(value)) value.toLong().toString() else "%g".format(value).trimEnd('0').trimEnd('.')

    // One choice can change what another offers: built afresh each time
    override fun rows(): List<ListRow> {
        val rows = mutableListOf<ListRow>()
        for ((index, section) in makeSections().withIndex()) {
            if (section.title != null) rows += ListRow.Header(section.title) else if (index > 0) rows += ListRow.Header("")
            for (row in section.rows) rows += item(row)
            section.footer?.let { rows += ListRow.Footer(it) }
        }
        return rows
    }

    private fun item(row: SettingRow): ListRow.Item = when (row) {
        is SettingRow.Toggle -> ListRow.Item(title = row.title, toggle = row.isOn(), onToggle = { row.set(it) })
        is SettingRow.Choice -> ListRow.Item(
            title = row.title,
            value = row.options.getOrElse(row.selected().coerceIn(0, maxOf(0, row.options.size - 1))) { "" },
            disclosure = true,
            onClick = {
                Dialogs.choose(requireContext(), row.title, row.options, row.selected()) {
                    row.set(it)
                    reload()
                }
            },
        )
        is SettingRow.Number -> ListRow.Item(
            title = row.title,
            value = if (row.unit.isEmpty()) text(row.value()) else "${text(row.value())} ${row.unit}",
            hint = "Opens a box to type a new value",
            onClick = {
                val bounds = "From ${text(row.range.start)} to ${text(row.range.endInclusive)}" +
                    if (row.unit.isEmpty()) "." else " ${row.unit}."
                var type = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
                if (row.range.start < 0) type = type or InputType.TYPE_NUMBER_FLAG_SIGNED
                Dialogs.prompt(requireContext(), row.title, bounds, text(row.value()), action = "Set", inputType = type) { typed ->
                    val number = typed.replace(',', '.').toDoubleOrNull() ?: return@prompt
                    row.set(number.coerceIn(row.range))
                    reload()
                }
            },
        )
        is SettingRow.FileChoice -> ListRow.Item(
            title = row.title,
            value = row.name().ifEmpty { "None" },
            hint = "Chooses a file",
            onClick = { chooseFile(row) },
        )
        is SettingRow.Info -> ListRow.Item(title = row.title, subtitle = row.detail)
        is SettingRow.Opens -> ListRow.Item(
            title = row.title, subtitle = row.detail, icon = row.icon, disclosure = true, onClick = { push(row.make()) },
        )
    }

    private fun chooseFile(row: SettingRow.FileChoice) {
        val name = row.name()
        val actions = mutableListOf("Choose a File")
        if (name.isNotEmpty()) actions += "Clear"
        Dialogs.actions(requireContext(), row.title, null, actions) { which ->
            if (which == 0) {
                main.pickFileCopy(row.mimeTypes) { file: File? ->
                    file ?: return@pickFileCopy
                    val ok = row.set(file.path)
                    file.delete()
                    reload()
                    if (!ok) tell(row.title, "That file could not be used.")
                }
            } else {
                row.set(null)
                reload()
            }
        }
    }
}

/**
 * The settings, a screen for each of the desktop Options window's tabs that means
 * something on a phone, in the same order and with the same options in each.
 */
class SettingsScreen : SettingsPageScreen("Settings", {
    listOf(SettingSection(null, null, pages.map { page -> SettingRow.Opens(page.title, page.detail, page.icon) { page.screen() } }))
}) {
    private class Page(
        val title: String,
        val detail: String,
        @DrawableRes val icon: Int,
        /** A screen of its own, instead of one made from `sections`. */
        val make: (() -> Screen)? = null,
        val sections: () -> List<SettingSection> = { emptyList() },
    ) {
        fun screen(): Screen = make?.invoke() ?: SettingsPageScreen(title, sections)
    }

    companion object {
        /** One of the screens by its title, for opening it directly. */
        fun page(titled: String): Screen? = pages.firstOrNull { it.title == titled }?.screen()

        // Shorthands for an option kept by name (settings_jni.cpp)
        private fun toggle(title: String, name: String) =
            SettingRow.Toggle(title, { Engine.number(name) != 0.0 }, { Engine.setNumber(if (it) 1.0 else 0.0, name) })

        /** A choice among values, each with its name: the option holds the value. */
        private fun choice(title: String, name: String, options: List<Pair<String, Double>>) = SettingRow.Choice(
            title, options.map { it.first },
            { options.indexOfFirst { it.second == Engine.number(name) }.coerceAtLeast(0) },
            { Engine.setNumber(options[it].second, name) },
        )

        private fun number(title: String, name: String, unit: String, range: ClosedFloatingPointRange<Double>) =
            SettingRow.Number(title, unit, range, { Engine.number(name) }, { Engine.setNumber(it, name) })

        private fun file(title: String, name: String, mimeTypes: Array<String>) =
            SettingRow.FileChoice(title, mimeTypes, { Engine.fileName(name) }, { Engine.setFile(it, name) })

        /** Spring's or tape's speed, from the speeds comma and period go through */
        private fun speed(title: String, name: String) =
            choice(title, name, listOf(2, 3, 4, 6, 8, 12, 16, 24, 32).map { "$it times" to it.toDouble() })

        private fun effect(title: String, identifier: Int) = SettingRow.Toggle(
            title,
            { Engine.effects.firstOrNull { it.identifier == identifier }?.enabled ?: false },
            { Engine.setEffect(identifier, it) },
        )

        private val pages: List<Page> = listOf(
            Page("Playback", "Volume, remembering, ReplayGain", R.drawable.ic_play) {
                listOf(
                    SettingSection(null, null, listOf(
                        toggle("Allow Volume Above 100%", "allowAmplify"),
                        toggle("Remember Playback State on Exit", "rememberState"),
                        choice("Remember Position if Longer Than", "rememberPosMinutes",
                            listOf("Off" to 0.0, "5 minutes" to 5.0, "10 minutes" to 10.0, "20 minutes" to 20.0,
                                "30 minutes" to 30.0, "45 minutes" to 45.0, "60 minutes" to 60.0)),
                        toggle("Load All Files in Folder When Opening a Single File", "loadFolder"),
                        SettingRow.Toggle("Play All Resumes Where You Left Off", { FolderResume.enabled }, { FolderResume.enabled = it }),
                        toggle("Auto-Advance to Next Playlist Item", "autoAdvance"),
                        toggle("Shuffle", "shuffle"),
                        choice("Repeat", "repeatMode", listOf("Off" to 0.0, "One" to 1.0, "All" to 2.0)),
                        number("Rewind on Pause", "rewindOnPauseMs", "ms", 0.0..60000.0),
                        choice("Volume Step", "volumeStepPercent", listOf(1, 2, 5, 10, 15, 20, 25).map { "$it%" to it.toDouble() }),
                    )),
                    SettingSection("ReplayGain", "Evens out loudness from the gain saved in a file's tags.", listOf(
                        choice("ReplayGain", "replayGainMode", listOf("Off" to 0.0, "Track" to 1.0, "Album" to 2.0)),
                        number("Preamp", "replayGainPreamp", "dB", -20.0..20.0),
                        toggle("Prevent Clipping", "replayGainPreventClip"),
                    )),
                    SettingSection("This Device",
                        "With Mix With Others on, FastPlay plays alongside other apps instead of stopping them, " +
                            "and the lock screen and headphone controls go to the other app. With Keep Screen On on, " +
                            "the screen does not dim and lock by itself while FastPlay is in front.",
                        listOf(
                            SettingRow.Toggle("Mix With Others", { Engine.mixWithOthers }, { Engine.mixWithOthers = it }),
                            SettingRow.Toggle("Keep Screen On", { Engine.keepScreenOn }, { Engine.keepScreenOn = it }),
                        ),
                    ),
                )
            },
            Page("Recording", "Format and bitrate", R.drawable.ic_record) {
                listOf(SettingSection(null,
                    "The Record button in the player records what is playing into the Recordings folder of " +
                        "FastPlay's files. The bitrate applies to MP3 and Ogg Vorbis. With Record With Effects off, " +
                        "recordings have the sound before the effects (tempo, pitch and rate still apply).",
                    listOf(
                        choice("Format", "recordFormat", listOf("WAV" to 0.0, "MP3" to 1.0, "Ogg Vorbis" to 2.0, "FLAC" to 3.0)),
                        choice("Bitrate", "recordBitrate", listOf(128, 192, 256, 320).map { "$it kbps" to it.toDouble() }),
                        toggle("Record With Effects", "recordEffects"),
                    )))
            },
            Page("Speech", "What FastPlay says", R.drawable.ic_mic) {
                listOf(SettingSection(null, "FastPlay speaks through TalkBack, while it is on.", listOf(
                    toggle("Announce Track Changes", "speechTrackChange"),
                    toggle("Speak Volume When Adjusted", "speechVolume"),
                    toggle("Speak Effect Value When Adjusted", "speechEffect"),
                )))
            },
            Page("Movement", "The seek amounts", R.drawable.ic_fast_forward) {
                listOf(SettingSection("Seek Amounts",
                    "The amounts Seek By goes through. Tracks need more than one track in the playlist, and " +
                        "chapters a file that has them.",
                    Engine.seekUnits.map { unit ->
                        val id = unit.identifier
                        SettingRow.Toggle(if (id == 12) "1 chapter (if available)" else unit.name,
                            { Engine.seekUnits.firstOrNull { it.identifier == id }?.enabled ?: false },
                            { Engine.setSeekUnit(id, it) })
                    }))
            },
            Page("Seeking", "Spring and tape seeking", R.drawable.ic_fast_forward) {
                listOf(
                    SettingSection("Spring",
                        "In touch mode, Seek Mode chooses spring or tape seeking. Spring plays through the audio at " +
                            "speed while held, the pitch kept, speeding up the longer it's held.",
                        listOf(
                            speed("Top Speed", "springSpeed"),
                            toggle("Speed Up the Longer It's Held", "springWindUp"),
                            number("Doubling Its Speed Every", "springDoublingMs", "ms", 50.0..5000.0),
                        )),
                    SettingSection("Tape",
                        "Tape plays faster and higher, like a tape. The tape stop slows what's playing to a standstill " +
                            "before it pauses or stops; the tape start speeds it up from one when it plays again. Times " +
                            "are 50 to 5000 milliseconds.",
                        listOf(
                            speed("Speed", "tapeSpeed"),
                            toggle("Spin Up When Held", "tapeSpinUp"),
                            number("Spin-Up Time", "tapeSpinUpMs", "ms", 50.0..5000.0),
                            toggle("Wind Down When Let Go", "tapeSpinDown"),
                            number("Wind-Down Time", "tapeSpinDownMs", "ms", 50.0..5000.0),
                            toggle("Tape Stop When Pausing", "tapeStopOnPause"),
                            toggle("Tape Stop When Stopping", "tapeStopOnStop"),
                            number("Tape Stop Length", "tapeStopMs", "ms", 50.0..5000.0),
                            toggle("Tape Start When Playing", "tapeStartOnPlay"),
                            number("Tape Start Length", "tapeStartMs", "ms", 50.0..5000.0),
                        )),
                )
            },
            Page("Effects", "Which effects are on", R.drawable.ic_settings) {
                listOf(
                    SettingSection("Stream Effects",
                        "With Wrap Around the Effect List on, the player's Effect control goes from the last " +
                            "effect to the first, and back.",
                        listOf(
                            effect("Volume (0 to 400%)", 0),
                            effect("Pitch (-12 to +12 semitones)", 1),
                            effect("Tempo (-50% to +100%)", 2),
                            effect("Playback Rate (0.5x to 2x)", 3),
                            choice("Rate Step", "rateStepMode", listOf("0.01x" to 0.0, "Semitone" to 1.0)),
                            toggle("Wrap Around the Effect List", "wrapEffects"),
                        )),
                    SettingSection("DSP Effects",
                        "The player's Effect control goes through the settings of the effects that are on. " +
                            "Convolution reverb needs an impulse response file.",
                        listOf(
                            choice("Reverb", "reverbAlgorithm", listOf("Off" to 0.0, "Simple" to 1.0, "Advanced" to 2.0)),
                            effect("Echo", 101),
                            effect("EQ (Bass, Mid, Treble)", 102),
                            effect("Compressor", 103),
                            effect("Normalizer (a steady level)", 108),
                            effect("Stereo Width (0 to 200%)", 104),
                            effect("Center Cancel (-100 to +100%)", 105),
                            effect("3D Audio (HRTF, Binaural)", 107),
                            effect("Convolution Reverb", 106),
                            file("Impulse Response File", "convolutionIR", arrayOf("audio/*")),
                        )),
                )
            },
            Page("Advanced", "Buffer, tempo algorithm, live streams", R.drawable.ic_settings) {
                listOf(
                    SettingSection("Audio Buffer",
                        "How far ahead audio is prepared: lower answers effect changes sooner, higher is safer.",
                        listOf(choice("Buffer Size", "bufferSize", listOf(100, 200, 300, 500, 1000, 2000).map { "$it ms" to it.toDouble() }))),
                    SettingSection("Tempo and Pitch",
                        "Speedy is made for speech, Signalsmith for music. A change applies on the next file.",
                        listOf(choice("Tempo and Pitch Algorithm", "tempoAlgorithm",
                            listOf("Speedy (Google)" to 1.0, "Signalsmith Stretch" to 2.0)))),
                    SettingSection("EQ Frequencies", "Changes apply the next time the EQ is turned on.", listOf(
                        number("Bass", "eqBassFreq", "Hz", 20.0..500.0),
                        number("Mid", "eqMidFreq", "Hz", 200.0..5000.0),
                        number("Treble", "eqTrebleFreq", "Hz", 2000.0..20000.0),
                    )),
                    SettingSection(null,
                        "A live stream kept for rewinding takes about 1 MB a minute at 128 kbps. Applies to streams opened after.",
                        listOf(
                            toggle("Smooth Seeking", "smoothSeek"),
                            toggle("Allow Rewinding and Pausing Live Streams", "liveRewind"),
                            choice("Rewind Length", "liveRewindMinutes", listOf(5, 10, 15, 30, 60, 120).map { "$it minutes" to it.toDouble() }),
                        )),
                )
            },
            Page("Speedy", "The speech speedup", R.drawable.ic_fast_forward) {
                listOf(SettingSection(null,
                    "Speedy uses a nonlinear speedup made for speech: it compresses vowels more than consonants, for clarity.",
                    listOf(toggle("Enable Nonlinear Speedup", "speedyNonlinear"))))
            },
            Page("Signalsmith", "The music time stretch", R.drawable.ic_notification) {
                listOf(SettingSection(null,
                    "Changes apply on the next file. A higher tonality limit keeps more harmonics when shifting pitch: " +
                        "0 for automatic, 4000 to 8000 for speech, 8000 to 16000 for music.",
                    listOf(
                        choice("Quality Preset", "ssPreset", listOf("Default" to 0.0, "Cheaper" to 1.0)),
                        number("Tonality Limit", "ssTonalityLimit", "Hz", 0.0..20000.0),
                    )))
            },
            Page("MIDI", "SoundFont and voices", R.drawable.ic_notification) {
                listOf(SettingSection(null, "MIDI files play through the SoundFont chosen here.", listOf(
                    file("SoundFont", "midiSoundFont", arrayOf("*/*")),
                    number("Max Voices", "midiMaxVoices", "", 1.0..1000.0),
                    toggle("Sinc Interpolation (Higher Quality, More CPU)", "midiSincInterp"),
                )))
            },
            Page("Auto Sync", "Folders synced to this device when FastPlay starts", R.drawable.ic_sync,
                make = { AutoSyncScreen() }),
            Page("About", "Version and audio engine", R.drawable.ic_info) {
                listOf(SettingSection(null, null, listOf(
                    SettingRow.Info("Version", Engine.version),
                    SettingRow.Info("Audio Engine", Engine.engineInfo),
                )))
            },
        )
    }
}
