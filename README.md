# FastRoute

Per-session Windows audio routing with a plain Win32 UI.

A **session** is a pipeline of **blocks**. You build it from the blocks library,
reorder it, and run it. Everything is keyboard driven and uses standard Win32
common controls, so screen readers announce it without any special handling.

## Licence

GPL version 3. See [LICENSE](LICENSE).

That follows from ASIO. Steinberg dual licensed the ASIO SDK on 15 October
2025 - either their own proprietary agreement, or GPLv3 - and FastRoute takes
the GPLv3 option, which means FastRoute is GPLv3 in turn. The alternative was
signing Steinberg's agreement and keeping the SDK out of the tree, which would
have meant anybody building this had to go and fetch it themselves.

Everything else FastRoute uses is MIT or BSD and therefore compatible; see
**Third party code** below and `docs/Third-party notices.txt`.

## Building

Requires Visual Studio 2022 (C++ desktop workload) and the Windows 10/11 SDK
(10.0.20348 or newer — per-process capture needs `audioclientactivationparams.h`).

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release

The executable lands in `build\Release\FastRoute.exe`.

## Blocks

| Category      | Block         | Parameters                                        |
|---------------|---------------|---------------------------------------------------|
| Input Blocks  | Application   | a named application or whatever is in front (its process tree is captured with it), volume |
| Input Blocks  | Input Device  | capture endpoint (or system default), volume      |
| Input Blocks  | System Audio  | output device to capture, applications to exclude, volume |
| Input Blocks  | Stream URL    | address, buffer, reconnect, volume                |
| Input Blocks  | Net Receive   | protocol, sender (discovered or typed), port, stream name, buffer, volume |
| Effects       | 10 Band EQ    | gain in dB for each of ten octave bands           |
| Effects       | 4 Band Parametric EQ | shape, frequency, gain and Q for each of four bands |
| Effects       | Bass and Treble | bass in dB, treble in dB                        |
| Effects       | Buffer        | how much to keep, skip step, save folder, template, format |
| Effects       | Channel Mixer | per channel volume and pan, stereo width, reverse channels, centre cancel |
| Effects       | Clipping Monitor | level that counts as clipping, least time between beeps |
| Effects       | Compressor    | threshold, ratio, attack, release, makeup gain    |
| Effects       | Gain          | gain                                              |
| Effects       | Limiter       | ceiling, release, lookahead                       |
| Effects       | Noise Gate    | threshold, cut, attack, hold, release, lookahead  |
| Effects       | Normaliser    | target level, lookahead, maximum boost, release   |
| Effects       | Pitch Shifter | semitones, cents, quality (which is also the delay) |
| Effects       | RNNoise Denoise | dry/wet mix                                     |
| Effects       | Speex Denoise | noise suppression in dB, dry/wet mix              |
| Output Blocks | Output Device | render endpoint (or system default), volume       |
| Output Blocks | Recorder      | folder, file name template, format, bitrate, silence gate, file splitting, volume |
| Output Blocks | Net Send      | protocol, group or host, port, stream name, codec, packet length, Opus bitrate, hop limit, volume |

The library tree groups these under **Input Blocks**, **Built-in Effects**,
**External Effects** and **Output Blocks**. External Effects is whatever CLAP
and VST3 plugins the last scan found - see **Plugins** below.

Blocks the running Windows cannot support are left out of the library tree
rather than offered and then refused - see **Windows versions** below.

Every block can also be given a **name** of its own, with F2 or from its
context menu. Two Gain blocks in one session are otherwise told apart only by
where they sit in the list. Clearing the name puts the block type back.

Every block also has an **enabled** flag. A disabled block stays in the
pipeline but is bypassed: a disabled input contributes nothing, a disabled
effect passes the bus through untouched, a disabled output is fed silence.
Toggle it with Space in the block list, or from the block's parameters dialog.

The blocks library tree lists the effects alphabetically. Inputs and outputs
keep the order above, which is the order you would usually reach for them in.

## Denoising

Two denoisers, because they are good at different things.

**Speex** is the SpeexDSP preprocessor. It runs at any sample rate natively
and takes a suppression strength in dB, so it is the one to reach for when you
want a specific, predictable amount of reduction. Measured here at about 18 dB
of reduction on broadband noise at its default setting.

**RNNoise** is a trained recurrent model. It has no strength control - the
model decides what to remove. It is built for pulling background noise out
from under speech, which is exactly the case a microphone block cares about.

The model itself is fixed at 48 kHz, and a session runs at whatever rate
Windows has its master output device set to, which is very often 44.1 kHz.
This used to refuse to start there and tell you to go and change the device,
which made the block unusable on ordinary machines for a reason that has
nothing to do with what the block is for. The bus is now converted to 48 kHz
around the model and back with the resampler that already ships with SpeexDSP.
A session already at 48 kHz gets none of that: the converters are never
created and the audio takes exactly the path it always took.

The conversion costs latency rather than quality. Measured out and back at
44.1 kHz, the response is flat to within a thousandth of a dB from 100 Hz to
12 kHz, and the round trip reconstructs a waveform to about 1%, nearly all of
which is the group delay not landing on a whole number of samples. What it
adds is 224 frames on top of the model's own 10 ms frame - about 5 ms at
44.1 kHz, for 15 ms in total against 10 ms at 48 kHz. That is the two
converters' group delay plus a little silence primed in front of the output so
that the odd sample of jitter between them can never leave it empty, and it is
paid twice because the priming has to cover the delay before it can hide it.

Both denoisers cost one frame of latency (10 ms), reported separately in the
session window; RNNoise reports the conversion on top of it where there is
one. Both run one instance per channel and blend the dry signal inside the
frame, where it is still aligned, so the mix control cannot comb filter.
Disabling a denoise block winds its mix to fully dry rather than skipping it,
so bypass is sample accurate and switching it back on does not spit out a
pipeline full of stale audio.

## System audio

The **System Audio** block captures everything playing on an output device, and
can leave named applications out of it. Those are two different captures under
the covers, because Windows offers no single way to say "this device, without
these applications":

* **With nothing excluded**, the endpoint itself is captured. That takes the
  device's whole mix in one stream, picks up an application the instant it
  starts playing, and works on every Windows FastRoute runs on.
* **With exclusions**, each application that is *not* excluded is captured
  separately and the results are mixed. That needs Windows 10 version 2004,
  and an application that starts playing joins the mix within half a second
  rather than instantly.

The applications considered are the ones playing **to the block's own device**,
not every application on the machine. That distinction is easy to lose, because
the per-application path does not use the endpoint at all - it captures
processes - and an earlier version of this simply took every audio process
there was. The effect was that adding a single exclusion to a block pointed at
a quiet virtual cable silently pulled in the browser, the mail client and the
screen reader, none of which were ever part of that device's mix. The device is
now used to decide whose audio belongs, which is what choosing it was for.

The one case this cannot narrow is an application playing to two devices at
once. Process loopback takes everything that process renders, wherever it is
going, so such an application arrives whole or not at all. Excluding it is the
way out, and the block's note says so.

Excluding an application excludes its child processes too, the same way
capturing one captures them. Names are matched case-insensitively against the
program file name, so `discord.exe` is enough. **Add Application...** takes a
name for something that is not currently playing, so it can be excluded before
it starts.

FastRoute is never in the mix on the per-application path, so there is no way
for a session to capture its own output. The endpoint path has no such
guarantee - it takes the device's whole mix, this session's contribution
included - so a session that captures a device *and* plays back to it is
refused rather than allowed to build into a feedback loop. Send the output
somewhere else, or exclude an application so the per-application path is used.

## Streams

The **Stream URL** block plays an internet audio stream as an input: give it an
http or https address and it behaves like any other source, with effects and
outputs below it.

Two decoders sit behind it, because no single one covers what people actually
stream:

* **Ogg Vorbis and Opus** are fetched over WinHTTP and decoded by libvorbis and
  libopus, which are built into FastRoute. Windows cannot be relied on for
  either - this machine has no Vorbis decoder at all, and Windows 7 has neither.
* **MP3, AAC, FLAC** and anything else Windows knows are handed to Media
  Foundation, which fetches them itself.

Which path is taken is decided by the first bytes of the stream, not the file
extension: an Icecast mount point rarely has one, and a `Content-Type` header
is only as honest as the server sending it.

**The buffer** is the delay you accept in exchange for riding out a bad
connection - 200 ms to 10 seconds, a second by default. It is far larger than
the 20 ms a sound card gets, because a network arrives in bursts over a link
that stalls, from a clock nobody here controls. **Reconnect** brings the stream
back after it drops, waiting the set number of seconds between attempts; each
failure goes to the error log rather than a message box, since a stream that
drops at three in the morning should not be waiting for someone to click OK.

A stream is written into its buffer a slice at a time, waiting for the mixer to
take what is already there. Without that, an ordinary file over HTTP arrives as
fast as the link allows and would overrun the buffer in seconds; a live stream
paces itself and never notices.

## Plugins

FastRoute hosts **CLAP** and **VST3** effects. Add the folders to search in
Settings, choose **Scan**, and what it finds appears in the blocks library
under **External Effects**.

Scanning means loading every module in those folders far enough to ask what is
inside - which is running other people's initialisation code - so it happens
when you ask for it rather than every time FastRoute starts. What it finds is
written into the document, so the library is there on the next run without
loading anything. It runs on its own thread and the dialog stays answerable
while it does; a module that will not load is skipped in silence, because a
plugin folder normally holds a good deal that is not a plugin.

Instruments are left out. Both formats let a plugin say what it is, and
FastRoute has nothing to feed a synth with. The two are harder to tell apart
in VST3 than it looks: instruments and effects share one category, and what
separates them is a subcategory string that only the second version of the
factory interface hands out. Without reading it, every synth on the machine
turns up in the effects library - which is exactly what happened here first
time round.

**Parameters come first.** A plugin's own window is a picture of knobs: usable
if you can see it, and nothing at all if you cannot. So the block's dialog
lists every parameter the plugin declares as text, with its value spelled the
way the plugin spells it - "-6.0 dB", "Low pass" - and edits it with a slider
or a combo box like any other setting here. A stepped parameter gets the combo
box, because a slider reading "2" tells a listener nothing.

**The plugin's own editor** is a button next to the list, for the parts of a
plugin its parameter list does not describe. It opens in a window of its own,
sized to what the plugin asks for, and closes with Escape. Whatever it changed
is read back out of the plugin when the dialog is closed, so the two ways of
editing agree.

The dialog loads its own copy of the plugin rather than reaching into the one
the audio graph is running, so changes are heard when OK or Apply is pressed
rather than while the slider moves. That is the same as every other block
dialog here, and it keeps the audio thread out of a dialog's lifetime. Apply
leaves the plugin's own editor open, since the point of applying is to carry
on working; OK closes it and reads it back first.

A plugin that has been uninstalled does not stop a session: the block goes
transparent, the error log says which plugin and why, and the settings are kept
in case it comes back.

What is not carried over is a plugin's state beyond its parameters. A reverb or
an EQ restores completely; a sampler with a file loaded will not. That is a
known limit of storing parameter values rather than the plugin's own state
blob, not a fault.

## More than one application in one block

An Application block can name several programs. The list is of checkboxes, so
tick as many as apply; **Add Application** names one that is not playing
anything at the moment, which is the ordinary case when a session is being put
together rather than used.

Each ticked program is captured with its own process tree, and the results are
summed into the one block. That is the same machinery the System Audio block
uses for its exclusions, and the set is rescanned while the session runs, so a
program named here that is not running yet joins the mix when it starts and
leaves it when it stops. A games block where only one game is ever running is
therefore normal rather than broken, and only none of them running at all is
worth an error.

### Starting before the thing you are recording

**Wait for them to start rather than refusing to run** removes that last error
too. A session set up to record a game is set up *before* the game is launched:
start the session, say what you are about to play, then launch it. Without this
the session refuses to start at all, so the talking has to happen after the
launch or not be recorded.

It works for a block naming one program as much as for a block naming ten. The
machinery is the same either way - the rescanning capture, which is what
already lets a second game join a block that is running - so the only thing the
option changes is whether an empty mix at the start is an error or a wait.

What it costs is the warning. A block that is told to wait cannot tell a program
that has not started yet from a name spelled wrongly: both are simply absent,
and the session runs and records silence either way. A block that is not told to
wait says *so-and-so is not running* and stops, which is the right answer while
a session is being set up and the wrong one while it is being used.

What this is for is a block per *kind* of thing rather than per program. A
recording session can have a microphone block, a screen reader block naming
every screen reader, and a game block naming every game, and each block is one
thing to set a level on, one thing to put an effect in, and - because the
Recorder follows the block list - one track.

Naming three programs in one block is not the same as three blocks naming one
each. Three blocks give three levels, three chains and three tracks; one block
gives one of each. Either is right, depending on which you want.

## What the application list contains

An Application block's list comes in three parts, in this order:

* the programs **playing audio** right now;
* the programs with a **window on screen**, playing or not;
* and the programs this block already names, whether or not either is true of
  them today.

Every row says which of those it is, so "running" and "playing audio" are told
apart by reading the row rather than by counting down the list.

The second part is there because the first one used to be the whole list, and a
program that is running quietly is not in it. Neither is a program whose only
sounds go through the Windows system sounds session: that session belongs to no
process, so there is nothing to attribute it to, and the programs that use it
are mostly older ones calling `PlaySound`. None of this has anything to do with
whether a program is 32-bit or 64-bit, which is what the report that prompted
the change suspected - a process's name is read the same way either way, and a
32-bit program with an audio session was always listed. What the missing
programs had in common was being quiet at the moment the list was built.

**Show every running program** adds the rest of the process table: the things
with no window and nothing to hear, services included. It is not the default,
because there are a few hundred of them and most are not applications in any
sense a person means, but it means a program that ought to be in the list and
is not can be found there rather than typed from memory.

**Add Application** is for a program that is not running at all, which is the
ordinary case when a session is put together before the thing it is going to
record. A name typed there stays in the list, marked *not running*, until it is
unticked.

FastRoute itself is never offered, and nor is anything it was started from:
capturing either would take FastRoute's own output with it and feed the session
back into itself.

## Which process is the one playing the sound

The list includes every process **holding an audio session**, window or no
window, so a helper process that plays audio without a window of its own is in
it and can be picked. That matters more often than it sounds,
because a lot of applications do not play their own audio.

Steam is the example worth knowing, found by David K:

* Capturing **steam.exe** gets every game launched from Steam, because Steam
  starts them and a block captures its target's whole process tree.
* Capturing **steamwebhelper.exe** gets Steam's own interface sounds, which
  come from that helper rather than from steam.exe.

So the two are different captures, and which is wanted depends on which sound
is wanted. Both can be in one session.

Anything built on a web view behaves this way, since the audio belongs to the
embedded browser process. If a block captures nothing, the thing to try is the
list itself: whatever is making the sound is in it by definition, under its own
name, while something is playing.

## Capturing whatever is in front

An **Application** block can be pointed at a process by name, as it always
could, or at the foreground:

* **Capture the application chosen below** is the original behaviour.
* **Capture whatever is in front when this block starts** decides once, when
  the session runs or when the block is switched back on, and then stays there.
* **Follow whatever is in front, switching as it changes** keeps up, moving to
  each application as it comes to the front.

Two things make this harder than one call to `GetForegroundWindow`, and both
are worth knowing about because they are what the setting is for.

**The foreground is tracked, not sampled.** By the time a session starts,
FastRoute is very often what is in front - the user just pressed Ctrl+R in the
FastRoute window. So the last application to have been in front is remembered
as it changes, and that is what a block asks for. The intended way to use this
is to hide FastRoute to the tray and start the session, or enable the block,
from a global hotkey: FastRoute never comes to the front, and the block takes
the application you were already using.

**Most of what comes to the front is not an application.** The Start menu, the
search box, the task switcher, the taskbar and the desktop all take the
foreground, and treating them as the new target would mean losing the audio you
were listening to every time you pressed a key. They are ignored and the last
real application stands. So is FastRoute itself - capturing it would feed the
session's own output back into itself.

A packaged application's window belongs to `ApplicationFrameHost.exe` rather
than to the application; the real process is found underneath it, because
capturing the frame host would capture nothing.

The session window's status line says what is actually being captured -
"Capturing firefox.exe" - which in follow mode is the one thing worth knowing
and has nowhere else to appear.

Switching capture is a real change of source, so there is a moment of silence
as one capture closes and the next opens. It is not counted as a dropout: that
would report a fault where there is only a decision.

## Tone, EQ and channel mixing

Three filter blocks, all zero latency and all bit exact when bypassed.

**Bass and Treble** is a pair of shelving filters: a low shelf at 100 Hz and a
high shelf at 4 kHz, each from -15 to +15 dB. It exists next to the ten band EQ
because most of the time two controls that do the obvious thing are what you
actually want.

**10 Band EQ** is one peaking filter per octave at 31, 62, 125, 250, 500 Hz and
1, 2, 4, 8, 16 kHz, each from -12 to +12 dB, with a Reset to Flat button. A
band whose centre lands at or above Nyquist is dropped rather than allowed to
fold back, so the 16 kHz slider simply does nothing on a 32 kHz endpoint.

One thing to know about it: like every graphic EQ built as a cascade, pushing
several neighbouring bands up by the same amount raises the level by more than
that amount, because the skirts of adjacent filters add. With every slider at
+6 dB the real gain is about +8.5 dB. Q was chosen by sweeping 0.8 to 4.0 and
taking the value where the worst deviation bottoms out (1.5, about 2.5 dB); it
cannot be made to vanish. Put a Gain block below the EQ to trim the result.

**4 Band Parametric EQ** is the other kind of equaliser: four bands, each a
whole filter rather than a level on a fixed grid. Each one has a **shape**
(peaking, low or high shelf, low or high pass, or off), a **frequency** from
20 Hz to 20 kHz, a **gain** of -18 to +18 dB, and a **Q** from 0.1 to 10. That
is what lets you take out one ring at 3.2 kHz, or roll everything off below
80 Hz - neither of which ten fixed bands can do.

The dialog shows one band at a time rather than sixteen controls at once. Each
row of the list is a whole band written out - "Band 2: Peaking at 500 Hz,
+3 dB, Q 1.0" - so the setting is in the row rather than spread across four
controls, and the controls below edit whichever band is selected. Gain is
unavailable on the two pass filters, which do not use it.

The frequency slider is logarithmic. Half of what anyone listens to lives below
1 kHz, and a linear 20 Hz to 20 kHz slider would put all of it in the first
twentieth of the travel. Frequencies are rounded to something worth reading
out: 4970 Hz is not a setting anyone means.

**Channel Mixer** is two stages. First each incoming channel gets a level and
a place in the stereo field, which is a 2x2 matrix of how much of each input
reaches each output. Then the result goes through one mid/side decomposition.

* **Volume**, per channel, -60 to +12 dB. The bottom of the slider is silence
  exactly rather than the millionth of full scale the arithmetic would give,
  because a control that says "all the way down" should mean off.
* **Pan**, per channel, hard left to hard right. The law is constant power:
  half the power to each side at the centre, full scale at either extreme, so
  a channel does not change loudness as it crosses the field. The defaults -
  each channel at 0 dB hard over on its own side - multiply by exactly one, so
  a mixer block that has only ever had its width touched is bit for bit what
  it was before these controls existed.

  Positions are said as a side and a distance - "hard left", "left 40",
  "centre" - in the dialog and in the block's one-line summary both. A signed
  number would be a reading to decode, and which half of the field the sign
  means is a convention rather than a word.

Volume and pan come first because they describe the source; the three controls
below describe the result. Panning both channels to the middle and then
cancelling the centre leaves silence, which is the arithmetic agreeing with
what the two controls say they do.

* **Stereo width**, 0 to 200%. 0% collapses to mono, 100% leaves the channels
  alone, 200% doubles the difference between them.
* **Reverse the left and right channels**, applied first, so it fixes the
  wiring before anything else looks at the signal.
* **Cancel the centre**, which removes anything panned dead centre - usually
  the lead vocal. What is left is out of phase between the two channels, so it
  disappears entirely if the audio is later summed to mono. That is inherent to
  centre cancellation, not a bug, and the dialog says so.

## OK, Apply and Cancel

Every block's parameter dialog has three buttons. OK reads the fields and
closes. **Apply**, on Alt+A, reads the same fields, pushes them into the
running session and stays where it is, which is the difference between tuning
a compressor against live audio and guessing at it one dialog at a time.
Cancel closes without applying anything further.

Applied changes stick. Cancel after an Apply undoes what was done since that
Apply, not the Apply itself - the Windows rule for these three buttons, and the
only reading under which Apply is worth having.

Apply beeps. The change is meant to be heard, but a setting that was already
where you wanted it makes no audible difference, and a button that might have
done nothing is indistinguishable from a broken one. If the change could not be
applied - the session refused the new graph, or the settings file would not
save - the error says so and there is no beep.

Both buttons run the dialog's own OK branch; only what happens at the end of it
differs. There is deliberately no second copy of the field reading that could
drift out of step with the first.

The Action Hotkeys dialog has no Apply. It is reached from inside the Buffer
block's dialog as well as from the block menu, so it does not always have a
session window underneath it to apply to.

## Compressor and limiter

Two dynamics blocks. Both are stereo linked: one gain is worked out from
whichever channel is louder and applied to both, because two independent
detectors would pull the stereo image sideways every time one channel got loud
on its own.

**Compressor** evens out the difference between the loud parts and the quiet
ones.

* **Threshold**, -60 to 0 dBFS, is the level above which it starts working.
* **Ratio**, 1:1 to 20:1, is how much of what is over the threshold gets
  through: at 4:1, 20 dB over comes out 5 dB over. 1:1 is not a compressor at
  all and passes every sample untouched.
* **Attack** (1 to 200 ms) and **Release** (10 to 2000 ms) are how fast it
  takes hold and how fast it lets go.
* **Makeup gain** is added afterwards, up to 24 dB. **Automatic** is the
  default and works out the figure that puts a full scale peak back at full
  scale - at -20 dB and 4:1 that is +15 dB - so the result is as loud at the
  top as it was and louder everywhere below. The dialog shows the number it
  arrived at rather than leaving it a mystery.

The knee is a fixed 6 dB either side of the threshold, so the ratio comes in
gradually instead of switching on at a point. A hard knee is audible on speech
as a sort of clicking on the consonants that cross the line.

**Noise Gate** is for the hiss and hum between the words, and the neighbour's
lawnmower in the gaps: below a threshold, it turns the signal down.

* **Threshold**, -80 to 0 dBFS.
* **Cut**, 0 to 80 dB - how far down it goes. A gate that shuts completely is
  very obvious; one that drops the background 20 dB often sounds like the noise
  was never there. A cut of 0 dB turns the gate off.
* **Attack**, **Hold** and **Release**. The hold is what keeps it open after
  the sound stops: without one, a gate closes on the tail of every word and the
  speech sounds clipped short.
* **Lookahead**, 0 to 50 ms, 5 by default on a new block.

It opens at the threshold but does not shut until the signal has fallen 3 dB
below it. That hysteresis is what stops a sound sitting exactly on the line
switching the gate on and off at the syllable rate. It also starts open, so the
first word is not swallowed while it works out that there is something there.

The lookahead solves the other end of the same problem. Without one, the gate
cannot know a word has started until its first sample has already gone past,
so the attack is still running when the consonant arrives and the front of it
comes out climbing into place - the very thing a gate is most often blamed for.
With a lookahead the audio is held back by that long while the detector runs on
what has not come out yet, so the gate is already open when the word emerges.

The detector takes the loudest sample anywhere in the window rather than the
one at the far end of it, tracked with a monotonic queue so it costs the same
per sample however long the window is. That means the gate opens the moment
anything loud enough enters the window, and does not begin to close until the
whole window has gone quiet.

Set it at least as long as the attack. Four time constants is 98% of the way,
so a 5 ms attack is arrived at 20 ms of lookahead and to the sample by 40.
Measured here: with 40 ms against a 5 ms attack, a burst after silence starts
at -0.01 dB of its own level; with no lookahead the first millisecond of it is
42 dB down.

The cost is latency, the same length, reported in the session window like any
other. A gate cut by 0 dB is doing nothing and reports none. A **bypassed**
gate keeps its delay: the engine disables a block by winding its mix to fully
dry rather than taking it out of the graph, so a delay that came and went with
the checkbox would take the session's latency with it. Bypass promises that
nothing is altered, not that nothing is held.

An existing session loads with no lookahead, because that is what it was built
and listened to with. New gate blocks start at 5 ms.

**Limiter** is the backstop: nothing leaves it above the ceiling.

* **Ceiling**, -12 to 0 dBFS.
* **Release**, 10 to 1000 ms.
* **Lookahead**, 0 to 20 ms, 5 by default.

Lookahead is what separates a limiter from a clipper. The audio is held back by
those few milliseconds so the gain can already be down by the time a peak
arrives, instead of catching up after it. It is the one effect in FastRoute
that adds latency, and it adds exactly the lookahead - which is why the control
says so, and why the session's latency readout goes up when you use it. At zero
there is nothing to look ahead at and the ceiling is enforced by clipping.

The ceiling is also clamped as a last resort after the gain is applied, because
"almost never above the ceiling" is not what a ceiling means. On a steady tone
that clamp never fires: the output is the input scaled by one number, which is
what the tests check.

The filter blocks, the dynamics blocks and the two denoisers all use the same
bypass rule: a disabled effect has its mix wound to fully dry rather than being
skipped, so its state keeps running and enabling or disabling it is silent. For
the limiter that means a disabled block still delays by its lookahead - the
latency of the graph does not jump about as blocks are switched on and off.

## Pitch shifting

The Pitch Shifter moves the pitch without moving the speed. That is not
something a filter can do: a filter changes what is there, and this has to
change where it is. The signal is taken apart into overlapping windows, each
window's spectrum is moved up or down, and the pieces are laid back down in the
places they came from - which is [Signalsmith
Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch), vendored
and pinned like everything else here.

* **Semitones**, -24 to +24. Two octaves either way is as far as anything stays
  recognisable.
* **Cents**, -100 to +100. A hundredth of a semitone each, and a full semitone
  either side, so any pitch in range can be reached from either direction: 3
  semitones and -50 cents is the same place as 2 semitones and +50.
* **Quality**, which is really a choice of how much delay to spend - best (120
  ms), balanced (60 ms) or low delay (30 ms).

The two sliders are one number, so the line under them says what that number
comes to: "up 2 semitones and 50 cents, delaying by about 60 ms". Reading back
"3 semitones" and "-50 cents" separately leaves whoever is listening to do the
arithmetic every time either slider moves.

### What it costs

A window. The block cannot say anything about a sound until it has a whole
window of it, so the delay is exactly the window and nothing can be done about
that - it is the shape of the problem rather than an implementation. 120 ms is
a long time in a live path, which is why the window is a setting.

A shorter window is less delay and less to work with. It resolves low
frequencies more coarsely, so a bass line goes grainy at 30 ms long before a
voice does; speech is the thing the low delay setting is for.

**At no shift at all the block costs nothing.** Zero semitones and zero cents
is not a shifter running at a ratio of one - it is a block that has been told
to leave the audio alone, so nothing is built, nothing is delayed, and the
latency it reports is none. The setting is one somebody arrives at by dragging
through it, so going in and out of it rebuilds cleanly rather than playing out
whatever the shifter was still holding.

A **bypassed** block keeps its delay, the same as the gate and the limiter: the
engine disables a block by winding its mix to fully dry rather than taking it
out of the graph, so a delay that came and went with the checkbox would take
the session's reported latency with it. Bypassed, what comes out is the input
delayed by exactly that much and altered in no other way, sample for sample.

The dry side of the **mix** is delayed to meet the shifted side. At 50% you get
a voice against its own undisturbed self, which is a harmony; without the delay
you would get a voice against a copy of itself 120 ms out of step, which is a
comb filter.

### What it does not do

It does not preserve formants. Shifted up, a voice goes up whole - the pitch
and the resonances of the throat that produced it together - which is the
chipmunk sound, and shifted down it is the same thing in reverse. Keeping the
formants where they are while the pitch moves needs a later version of
Signalsmith Stretch than the one pinned here; that version splits its FFTs out
into a second library, which would be a second checkout to pin and keep in
step. Worth doing if the voice matters; not done yet.

## Networked audio

**Net Receive** is an input block and **Net Send** is an output block, and
together they move audio between machines over the local network. They speak
two protocols.

**FastRoute's own** is for talking to another copy of FastRoute. A sender
announces itself on a multicast group twice a second, so a Net Receive block
lists what it has heard - "Desk mic on DESKPC - Opus, 48 kHz stereo,
192.168.1.40" - and choosing a row fills in the rest. Nobody has to know an
address. What is remembered is the **name**, not the number: a sender that is
restarted comes back with a new stream id, and somebody who chose "Desk mic"
yesterday meant the thing called Desk mic today, so the receiver follows the
name across the restart.

**VBAN** is VB-Audio's protocol, which VoiceMeeter, VBAN Receptor on a phone
and a good deal of other software already speak. It is what makes this useful
outside FastRoute. VBAN is found the other way round - by being asked rather
than by announcing - so FastRoute sends an identification request to every host
on the subnet and collects the answers, and a running Net Receive block answers
the same request so that a VoiceMeeter can find it. Both kinds of sender end up
in the same list.

A VBAN stream is identified by its **name** rather than by an id, because one
VoiceMeeter sends several streams to one port and the name is all that tells
them apart. Leave the name blank to take whatever arrives.

### A group, or one host

* **To a group** is what "broadcasting" means: one stream, and any number of
  receivers can join it. It costs the sender the same whether one machine is
  listening or six. The default group is 239.255.77.21, in the administratively
  scoped block, which is the right place for something that must not leave the
  site.

  A group is a **local network** mechanism and nothing else. No internet
  provider carries multicast, so a group cannot reach a machine somewhere else
  however it is addressed; and a group address has to be between 224.0.0.0 and
  239.255.255.255, so a host name does not name one. A Net Send block set to a
  group and given a host name refuses to start and says which of the two it
  thinks it was given. Some consumer routers also handle multicast badly even
  within one house.
* **To one host** works anywhere a packet can reach: across subnets, over a
  VPN and over the internet.

  **The receiver does the asking.** Leave the sender's address blank and it
  waits on its port; a Net Receive block pointed at that machine asks it for
  the audio once a second and the sender sends to whoever asked. So the way to
  set one of these up is the way anybody would expect: start the sender, then
  go to the other machine and name the one that is sending. The sender is told
  nothing about who wants it.

  Two things fall out of that. The asking is outbound from the receiver, so the
  reply comes back down a path the router has already opened and **no port has
  to be forwarded on the receiving side**; only the sender's port has to be
  reachable. And a receiver that is closed, or whose machine goes away, simply
  stops asking, so the sender forgets it after six seconds without anybody
  having to say goodbye.

  Filling the sender's address in does the opposite - it pushes at that
  address whether or not anything is listening - which is for a receiver that
  cannot ask, such as one that is not running yet.

  An ask also requests the sender's own description, because the multicast
  announcements do not cross a subnet and a pulled stream would otherwise have
  no way to say what it is. That is why a Net Receive block pointed across the
  internet can still report the codec and the rate before any audio arrives.

  **Nothing here is encrypted or authenticated** - anything that finds the port
  can listen to what is being sent, and anything that can reach it will be
  taken as the sender. That is fine on a network you control and worth thinking
  about on one you do not.

The **hop limit** is how many routers a **group** packet is allowed to cross,
and it applies to nothing else. One keeps it on the local subnet, which is
almost always what is wanted. Zero keeps it inside this machine, which is how
to try the pair out on one PC. A packet sent to one host keeps the system's own
TTL instead: the two are not the same question, and the answer that is right
for a group - one hop, stay here - is exactly wrong for a machine anywhere
else, which the first router would drop. The control is greyed out when a block
is sending to one host, rather than appearing to do something it does not.

### Codecs

| Codec | What it is for | At 48 kHz stereo |
|-------|----------------|------------------|
| 16 bit PCM | the default; nothing to go wrong | 1536 kbps |
| 24 bit PCM | more depth than anything will ever need | 2304 kbps |
| 32 bit float PCM | the bus exactly as it stands, headroom above one included | 3072 kbps |
| FLAC | lossless, and about a third smaller than 16 bit PCM | around 1000 kbps |
| Opus | lossy and small enough for anything | whatever it is set to |

Opus has a dialog of its own behind **Opus settings** on a Net Send block,
because it has more to it than a bitrate and the rest is set once per link and
then left alone:

* **Protect against loss of** 0 to 50%. Opus puts a low rate copy of each
  packet inside the next one and spends part of the bitrate on it, so a lost
  packet can be rebuilt rather than guessed at. Zero turns it off and stops
  paying for it. This is the single most useful control on a link that drops
  things.
* **Complexity**, 0 to 10. Processor time on the sending machine against
  quality. 10 is Opus's own default and is not expensive next to the rest of a
  session.
* **What it is carrying** - work it out, music, or speech. Opus tunes itself
  quite differently for the two and guesses well, so the default is to let it.
* **Bitrate mode** - variable, variable within one packet size, or constant.
  Constant makes every packet the same size, which is easier to plan a
  congested link around even though it is worse per bit.
* **Send almost nothing while the input is silent.** Saves a great deal on a
  mostly quiet microphone. The silence then stops arriving rather than arriving
  as silence, which anything measuring the far end will notice.

FLAC here is 16 bit, because FLAC is integer only and at 16 bits one frame fits
one datagram even in the worst case where the audio does not compress at all.
Anything wanting more depth has 24 bit PCM a line above it.

Opus works at five sample rates and a session runs at whatever its master
endpoint runs at, which is often 44100. So the Opus encoder converts to 48 kHz
inside itself and says 48000 on the wire; the receiver converts whatever the
wire says back to the session rate, which it has to do anyway - a 44.1 kHz
sender playing to a 48 kHz receiver is an ordinary thing rather than a strange
one.

A VBAN stream is 16 bit PCM whatever the codec says, because PCM is what VBAN
carries: its other two codec slots are VB-Audio's own and are not implemented
here. The dialog shows this rather than hiding it, and so does the block's
one-line summary.

### Packets, and what a packet can hold

A packet has to fit one datagram. A fragmented audio packet is one the network
can lose in halves, and half a packet of PCM is not half as good - it is a gap
in the middle of the audio. With a 1500 byte MTU that leaves about 1400 bytes
of payload, so:

* 16 bit stereo at 48 kHz maxes out at **350 frames, about 7.3 ms**
* 24 bit at **233 frames, 4.9 ms**
* 32 bit float at **175 frames, 3.6 ms**
* FLAC at **334 frames**, sized for the worst case rather than the usual one:
  a block FLAC cannot predict is stored verbatim at the full four bytes a
  frame, and a packet that only fits when the audio happens to be compressible
  is a packet that fails on applause
* Opus takes one of six fixed frame lengths, so a request is rounded to the
  nearest of them

That is the network rather than a setting, so the packet length control says
what it really settled on - "10 ms asked, 350 frames (7.2 ms)" - instead of
leaving somebody to wonder why ten behaves like seven.

### The clock, and the buffer

Audio arriving over a network comes in bursts, from a machine whose sound card
is not this one and never will be exactly in step with it, over a link that
drops things and reorders them. The ring in front of the mixer handles the
clock part: three times the chosen buffer of capacity, a working depth of one
buffer, and a drift resampler that corrects for two clocks pulling apart. The
stream block has used it for internet radio since 0.1.

In front of that sits a **jitter buffer**, which is the part specific to a
network. Packets are held there until it is their turn rather than used the
moment they land, for three reasons.

**They arrive out of order.** UDP is allowed to reorder, and a wireless link or
anything with more than one path through it does. Using a packet the moment it
lands means the one that overtook gets played early and the one it overtook is
thrown away as stale - so one reordering costs two packets instead of none.

**They arrive late.** A packet three milliseconds behind its neighbour is not
lost, and with sixty milliseconds of buffer there is no reason to give up on it
after three. A gap therefore waits for a deadline - half the buffer - before it
is declared missing.

**And when one really is missing, the hole has to be exactly the right
length.** This is the part that matters most, and it is not obvious why. The
ring works out clock drift from how many frames went in against how many came
out. Frames quietly dropped instead of replaced look exactly like a sender
running slow, so the drift correction engages and resamples the whole session -
which is heard as **the pitch wandering about**, and as the loss varies, the
wander varies with it. The sender's own 64-bit frame counter is in every
packet, so the size of a gap is known exactly rather than estimated, and
filling it exactly is what keeps the pitch still.

A duplicate - UDP may send one twice - is dropped, because playing it would put
the stream permanently ahead of itself. A packet that arrives after its turn
has passed is counted and dropped, since putting it in would play it after
audio that comes later.

Where a gap is exactly one packet and the codec is Opus, the packet after it is
asked to rebuild it: Opus can carry a low rate copy of each frame in the
following packet, and the receiver asking for that is the difference between
rebuilding a lost packet and guessing at one. Whether the redundancy was
actually present cannot be known from the receiving side - Opus conceals
instead and reports success either way, and the libopus vendored here predates
the call that would answer - so nothing counts recoveries. It is worth asking
for regardless; a figure that could not be told from concealment would be a
figure flattering itself.

A silence longer than five buffers, or three seconds, is a sender that has gone
away rather than a network hiccup, and the stream is restarted instead of
having seconds of silence concealed into it. That threshold is deliberately
generous because Opus's discontinuous transmission deliberately sends almost
nothing while its input is silent, and a short threshold would restart the
stream every time somebody stopped talking.

### Why it was pitchy, which took three goes to find

The first symptom was the pitch wandering about on a poor link, and the cause
turned out to be four separate things in the ring, three of which had nothing
to do with the network code on top of it. Worth writing down, because the chain
was not obvious and the first two fixes did not stop it.

**The gap in a lost packet was dropped rather than filled.** The ring works out
clock drift from frames in against frames out, so frames missing look like a
sender running slow. Fixed by filling gaps to exactly the length that went
missing, from the sender's own frame counter.

**A dry read biased the drift estimate upward.** The estimate skipped any read
where the ring had run empty - including that read's output - while the audio
that arrived immediately afterwards was still counted in. A network source runs
dry on any hiccup, so the estimate climbed. Now a read only counts as idle when
there is nothing in the ring *and* nothing arriving.

**One glitch was counted per dropped frame rather than per overflow.** A burst
arriving after a stall is a single event, and it was being counted in the
hundreds - twelve seconds of a stalling link reported three and a half thousand
glitches for six stalls. That mattered for more than the tally, because the
drift correction latches on at five glitches: a single overflow of five frames
turned the resampler on for the life of the stream.

**And the correction's authority was enormous.** It was allowed five percent,
which is most of a semitone, with a gain that let one packet's worth of buffer
depth ask for one and a half percent. On a packetised source the depth moves by
a packet all the time, so once latched on, the correction rode it and the pitch
wandered. What this corrects is two clocks differing, which for real crystals
is tens of parts per million, so it is now clamped to a quarter of one percent
- about four cents, below hearing it as pitch - with a gain fifty times
gentler. The buffer absorbs jitter; the resampler is only for drift, and it
should be too slow and too weak to be noticed doing it.

There was also only one buffer of headroom above the working depth, so the
catch-up burst after a stall overflowed and the oldest audio was discarded
frame by frame. There are now two, and latency that does creep up after a stall
is walked back down by the correction rather than by throwing audio away.

Measured on a link stalling for 120 ms every two seconds, with a 60 ms buffer:
3709 glitches and 14.85 cents of pitch deviation before, 10 glitches and 0.00
cents after. The ten remaining are honest - a stall longer than the buffer
means audio that is simply not there, and nothing invents it back. That is what
a bigger buffer is for, and what the block now reports so the figure can be
chosen rather than guessed.

### What the block says about the link

A Net Receive block reports what the network is doing to it, because on a link
that is misbehaving the only thing worse than the audio being wrong is not
being able to tell why:

    Desk mic from 192.168.1.40, 48000 Hz Opus; 0.42% lost, 7 out of order; held up to 23 ms

Loss is counted in **frames**, not packets, so it means the same thing whatever
the packets are sized at. "Held up to" is how long a packet had to wait before
its turn came - which after a lost one includes the deadline spent hoping it
would still arrive, so it runs larger than the network's own unevenness and is
exactly the figure to size the buffer from. It is deliberately not called
jitter for that reason.

### Where the work happens

The render thread does none of it. A Net Send block drops frames into a queue
and wakes a thread that does the encoding and the sending, because `sendto` can
block for as long as the stack feels like and a render callback that is late is
a dropout on every output in the session, not just this one. The queue is capped
at half a second: a queue that grew without limit would turn a stalled network
into a memory leak, and when it fills the oldest audio is the least useful thing
to keep.

A Net Send block clocks nothing, the same as a recorder, so a session whose only
output is a Net Send block still gets a device opened purely to drive the graph.

## Builds, and updating itself

Every push to `main` builds on GitHub and replaces one release, tagged
`latest`, with the result. There is only ever one of those, so the download
link never goes stale. The workflow is `.github/workflows/build.yml`.

It also checks the binary for imports Windows 7 does not have. That check was
being done by hand until now, and it is the one mistake that stops FastRoute
starting at all on its own stated floor - not with an error, just nothing
happening when somebody double-clicks it.

The version lives in `src/version.h` and nowhere else. The VERSIONINFO
resource, `dist.ps1`'s default zip name and the release label all read it from
there, so a release cannot be labelled with one number and built from another.
CMake also bakes the short commit in, because two builds of 0.6 are otherwise
indistinguishable and between releases that is every build.

### Updating

**Help > Check for Updates** asks now. **Settings** has how often to ask by
itself - never, when FastRoute starts, hourly or daily - and whether to get on
with it without asking first.

Off by default. A program that phones home unasked should be asked first, and
Settings is where to ask.

What it compares is the commit where it can and the version where it cannot.
The commit is exact; the version is the fallback for a build that was made
without git to ask, and it is compared the way a person reads it, so 0.10 is
newer than 0.6 rather than older as a plain string comparison would have it.

Applying one is a batch file, because an exe cannot overwrite itself while it
is running: FastRoute writes the script, asks itself to close, and the script
waits, unpacks the zip over the folder, starts the new copy and deletes itself.
Nothing in the zip is a settings file - it holds the exe and the documentation
and nothing else - so sessions, settings and recordings are left where they
are. Nothing is installed and nothing in the registry is touched, which is the
same reason the whole folder can live on a memory stick.

A scheduled check that fails says nothing and writes to the error log instead.
A laptop that was asleep, or a network that is not up yet, is the usual reason,
and a box about it would be worse than useless.

## ASIO

An **ASIO Input** block and an **ASIO Output** block, for an interface with an
ASIO driver. Pick the driver, pick which two of its channels are the left and
the right, and that is the block.

ASIO is a different shape from WASAPI in ways that show through, so it is worth
saying what they are rather than letting them be surprises.

**The driver owns the clock.** It does not hand over audio when asked; it calls
FastRoute when it is ready, at a buffer size it chose. That is the whole reason
ASIO is low latency. When an ASIO output is the first output block in a session,
it drives the session, and there is nothing buffered between the mix and the
hardware at all.

**Input and output are one callback.** A driver presents both halves of an
interface together, so an ASIO input and an ASIO output on the same driver are
not two devices - they are two views of one callback. Audio in and audio out
therefore share a clock exactly: nothing is resampled and nothing drifts. That
pairing is the one worth reaching for.

**ASIO mixes with WASAPI freely.** An ASIO input into a Windows output, or a
Windows input into an ASIO output, both work. Whichever output block comes
first owns the clock and the rest run from a buffer, which is how two Windows
outputs have always worked here; an ASIO block is simply another block. Those
are two unrelated clocks, so that path uses the same buffer and the same slow
drift correction as a network stream or a second sound card - which is to say
it works, and it is not as tight as ASIO to ASIO on one interface.

**One driver per session.** ASIO keeps a single driver per program, and most
drivers allow a single program anyway. Two ASIO blocks have to be on the same
interface; a session asking for two different ones is refused with a reason.

**The sample rate is the one real catch.** A Windows device resamples to its own
format for nothing, so a WASAPI block runs at the session's rate whatever the
hardware is doing. ASIO does not: it hands over the hardware's rate and that is
that. FastRoute asks the driver to change, and most will, but one locked to an
external clock will not. Then either set the rate in the driver's own control
panel, or put the ASIO output first among the outputs so the session follows
the hardware instead. What will not happen is playback at the wrong speed:
a mismatch that cannot be resolved is refused and says which rate to set.

**Buffer size lives in the driver's panel**, reached with the button in the
block, because that is where ASIO puts it and where every other host sends
people. The dialog reads back what the driver reports - channel counts, rate,
buffer size and what that is in milliseconds - so there is no need to open the
panel to find out.

The driver is opened only when it is needed: listing the installed drivers
reads the registry, and nothing takes the hardware until a driver is actually
picked or a session starts. Stopping a session closes it again, because an
interface held open is one nothing else can use.

## Block actions, and the buffer

Everything else a block can be told is a **parameter**: a value written into
the document, with the running graph rebuilt around it. An **action** is not
that. "Rewind ten seconds" is not a value the block has, it is something done
to the block while it is playing, and it means nothing at all when the session
is stopped.

So actions take their own path. A hotkey bound to one reaches the live effect
through the engine rather than through the document, and what it changes is
never saved - only the hotkey itself is. A block type declares which actions it
has; **Action Hotkeys...** on the block menu is enabled for the blocks that
have any and dead for the rest, rather than appearing and disappearing as the
selection moves.

Each action gets its own key, and each key is global or window scoped like any
other. Pressed while the session is stopped there is nothing to act on, and the
low tone says so - the same 800 Hz for done and 400 Hz for not that every other
hotkey answers with.

**Buffer** keeps the last so many seconds of what passes through it and lets
you move around inside that while the session carries on. Audio keeps arriving
in real time and keeps leaving in real time, so being twelve seconds behind is
a fixed offset rather than a debt that grows: the window slides and the read
point slides with it.

* **How much to keep**, from ten seconds to ten minutes. The memory is the
  length times the rate times two channels times four bytes - a minute at
  48 kHz is 23 MB - which is why there is a maximum at all.
* **How far a rewind or fast forward moves**, from one second to two minutes.
* **Where a save goes**, with the same folder, template, format and bitrate
  fields a Recorder block has. It is the same question, so it is the same
  controls and the same code behind them.

Its actions are **Rewind**, **Fast Forward**, **Jump to Live** and **Save
Buffer**.

At live the offset is zero and the block is a passthrough that costs nothing -
the sample written this frame is the sample read this frame. That is worth
saying because it is what makes the block safe to leave in a chain: it adds no
latency at all until you actually ask it for some.

Two things are less obvious.

**Moving the read point is crossfaded** over a few milliseconds. Jumping a ring
buffer pointer joins two unrelated waveforms, and that join is a click every
single time. The fade is short enough not to be an effect and long enough to
remove the click.

**Saving does not interrupt anything.** The audio thread only sets a flag; the
copy out of the ring is made by the block's own thread, a few thousand frames
at a time, so the render callback never waits on a file or on twenty megabytes
of memcpy. Rewinding does not change what is saved either - a save writes
everything the buffer is holding, oldest first, wherever you happen to be
listening from.

Rewinding further back than the block has actually kept is clamped to what it
has. The ring is allocated full of zeroes, and without the clamp a rewind on a
session two seconds old would be a rewind into silence.

## Normalising, and watching for clipping

**Normaliser** is the running form of normalising a file. Normalising a file is
a two pass job: find the loudest point, then scale the whole thing so that
point lands on the target. A live stream has no second pass and no end to find
the loudest point of, so here the gain follows the signal instead of being
worked out once.

* **Target level** is where the loudest part is brought to.
* **Lookahead** is how far ahead it looks, and is exactly the delay it adds.
  The gain applied to a sample comes from the loudest sample in the window
  ahead of it, so the level is already down when a loud passage arrives rather
  than being caught out by it.
* **Maximum boost** is the ceiling on how far it will turn anything up. This is
  the control that makes the difference between a normaliser and an
  unusable one: room tone at -70 dB is worth 67 dB of boost on the way to a
  -3 dB target, and without a cap the pause between two sentences arrives
  louder than the sentences.
* **Release** is how fast the gain is allowed to come back up. Coming down is
  immediate, because a peak that is not caught is a peak that clips; going up
  is slow, so the gain does not audibly pump. They are deliberately asymmetric.

Silence is held rather than lifted. Without that, the gap between two sentences
would be filled with whatever the noise floor is, brought up to the target.

**Clipping Monitor** does not change the audio at all - it reads the buffer and
writes nothing back, so the block can sit anywhere in a chain without being
part of it. It is a meter you do not have to be looking at, for the case where
the first sign of trouble is otherwise a ruined recording found afterwards.

* **Level that counts as clipping**, from -12 dB to full scale. Below full
  scale it is an early warning rather than a report.
* **Least time between beeps**. A beep per buffer on a badly clipping signal
  would be a continuous tone.

The beep goes to the **default output device** through `Beep()`, deliberately
not into the session's own bus. Nobody downstream hears it: not the recording,
not the stream, not the other end of a call. The one exception is a session
that is itself capturing system audio or capturing this process, which picks
the beep up like any other sound on the machine - so a monitor inside a session
like that is worth thinking about twice.

`Beep()` blocks for its whole duration, which on the audio thread would be an
underrun every time it fired. The audio thread only ever sets a flag; a thread
of the block's own does the beeping, and polls for that flag rather than being
signalled, because signalling it would mean a system call from the render
callback to save a few milliseconds nobody is waiting on. Bypassing the block
stops it watching, rather than leaving it beeping silently in the background.

## Knowing what is going on

Two read-only fields, both edit controls rather than labels, because a screen
reader can walk an edit a line at a time and cannot walk a label at all.

The **session window** says how long the session has been running, and if a
recorder block is in it, how long has actually been recorded, into how many
files, and how large the file being written is now. Recorded time is not the
same as running time - the silence gate can hold off for most of a session, and
the difference is the whole point of it.

Blocks with something to say for themselves add a line each. A buffer that has
been rewound is the case this exists for: there is nothing else on screen that
would say so.

The **main window's Sessions tab** lists every session that is running and how
long each has been going. Both update once a second, and only when the text has
actually changed, so nothing is announced twice a second for saying the same
thing.

## When devices disagree about rate or bit depth

They are made to agree, and nothing in the session has to be told about it.

Inside FastRoute every sample is **32-bit float, stereo**, at one rate for the
whole session: **the first output block's own rate**, read from that endpoint's
mix format. That block is the master clock, so it is the one endpoint that
needs no conversion at all.

Everything else is converted at its edge:

* **Bit depth never reaches the mix.** Every endpoint, in or out, is opened
  asking for float32, and Windows converts the device's own 16, 24 or 32-bit
  samples on the way through. There is nothing to configure and nothing to get
  wrong: a 16-bit microphone and a 24-bit interface in one session are both
  float by the time a block sees them.
* **Sample rate is converted at every endpoint but the master.** A 44.1 kHz
  microphone in a session whose output runs at 48 kHz is resampled by Windows
  into 48 kHz; a second output running at 44.1 kHz is resampled back down on
  the way out. The session stays at one rate throughout, so no block ever sees
  two rates at once.
* **Reordering the blocks can change the rate.** The master is whichever output
  block comes first, so moving a 96 kHz interface above a 48 kHz one re-runs
  the whole graph at 96 kHz. The session window says which rate it is actually
  running at, and that is worth a look if a session sounds different after a
  reorder.

The session window's status line reads *Running at 48000 Hz* - the real figure,
not the setting - so you can always check what you got.

### Matching rates is still worth doing

Converting is not free, and two kinds of cost are worth knowing about.

The first is quality, and it is small: one resampling pass at the edge of the
graph, in Windows' own converter.

The second is more interesting. **Two devices never run at exactly the same
rate even when they agree on paper.** A USB microphone's 48 kHz is its own
crystal's 48 kHz, not the motherboard's, and the two drift apart by a fraction
of a percent - which is enough to overflow or starve a buffer over a few
minutes. FastRoute handles this by resampling continuously by the ratio it
*measures* between frames in and frames out, rather than by the rate either
device claims, so drift is absorbed instead of accumulating into a dropout.
That machinery runs whether or not the nominal rates match, and the status line
reports it as *Input resampled 1.002x* when it is doing anything appreciable.

So: mismatched rates work, matched rates work slightly better, and either way
the thing to watch is the dropout count rather than the numbers on the
hardware.

### Recording is the one place you choose

A recorder block can be set to a rate and bit depth of its own, independent of
whatever the session is running at - see **Rate and bit depth** under
Recording. That is the only place in FastRoute where either is a choice, and
it is a choice about the file rather than about the session: the graph keeps
running at the master output's rate whatever a recorder is set to.

## Recording

A Recorder is an output block, so it captures the bus exactly where it sits in
the list: put one above the denoisers and one below to record the raw and
cleaned signal from the same session.

* **Formats:** WAV, FLAC, MP3, AAC (in an M4A file), Ogg Vorbis and Opus.
  WAV is written directly; Ogg Vorbis and Opus by the same libraries the stream
  block decodes with, so they work on every Windows FastRoute runs on; FLAC,
  MP3 and AAC by the encoders Windows ships, so those depend on the release.
  A format this machine cannot encode is left out of the list rather than
  offered and then refused. The bitrate applies to the four lossy formats.
* **Sample rate and bit depth** can be set per block, or left to follow the
  session and the format - see **Rate and bit depth** below.
* **File name templates** expand `{session}`, `{block}`, `{index}`, `{date}`,
  `{time}`, `{datetime}` and `{n}`. `{index}` is the block's place in the
  pipeline, counted from one - so `{index}. {block}` writes `1. Mic.flac`,
  `2. Screen reader.flac`, `3. Game.flac`, and the files sort into the order
  the blocks are in. It is padded to the width the session needs, so a session
  of twelve blocks numbers them `01` to `12` and the ninth still sorts before
  the tenth. `{blocknumber}` is accepted as the same thing. A recorder's own
  mixed file is of no one block, so `{block}` and `{index}` are both empty
  there; for the per-block files below they are the point of the thing. An
  existing file is never overwritten: `{n}` becomes the next free number, and
  without it a ` (2)` suffix is appended. The extension is added
  automatically.
* Leaving the folder blank records into `Music\FastRoute`.
* **Ctrl+Shift+O** opens the recording folder in Explorer, from either window.
  In the session window a selected Recorder block wins, so a session with more
  than one opens the one being looked at; otherwise it is the session's first.
  The folder is not created just for being asked about - a recorder that has
  never run says so instead.
* **Settings** has a **default folder for new Recorder blocks**. A new recorder
  starts there instead of on the Music folder, which is the difference between
  setting the path once and setting it every time. It is a starting point and
  not a binding: the block keeps its own copy, so changing the default later
  leaves the recorders that already exist exactly where they were. A folder
  that is not there at the moment is queried rather than refused, since a path
  on a drive that is not plugged in yet is a legitimate thing to want.
* Encoding happens on its own thread behind an unbounded queue. The file system
  and the encoders both block, and neither belongs anywhere near the mixer.
* A session whose only output is a Recorder still works: with no output block
  to clock the graph, one is opened on the default device purely to drive it,
  playing silence.

### Pausing and resuming

There is no pause button, because the block already is one: **switch the
Recorder block off and on again**. The engine simply stops handing it audio,
and the file stays open at the position it had reached - so it is a pause
rather than a stop and a new file. It works from a hotkey, from whatever you
are doing at the time, with the high and low tones to say which way it went.

The one thing to know is that a pause is *absent* from the file rather than
silent in it, so a recording made with pauses is not a timeline. That is the
same thing **Skip silence** does, for the same reason.

### A file per block

**Recording mode** on a Recorder block offers three answers:

* **The mix, in one file** - what a recorder has always written: everything
  reaching that point in the list, through every effect above it.
* **Each input block, in a file of its own** - one file per input block above
  the recorder, named after the block.
* **Both** - the mixed file, and the per-block files beside it.

A per-block file is **not a slice of the mix.** It is taken at the input block
itself: that block's own volume applied, and nothing below the block in the
list. An effect between the input and the recorder is in the mixed file and not
in the tracks, which is usually the point - a multitrack recording is worth
having because each track is still raw enough to work on afterwards - but it
does mean the tracks do not add up to the mixed file when there are effects
between them.

They do line up with each other, exactly, and that is enforced rather than
hoped for:

* a block that is switched off writes **silence** into its track rather than
  nothing, so every track stays the same length;
* **Skip silence** is not applied to tracks. It decides per file, so a track
  that happened to go quiet would come out shorter than the ones that did not;
* **splitting by megabytes** is not applied to tracks either, because how many
  megabytes a track has made depends on what was in it. Splitting by *minutes*
  is the same number of frames for everybody, so that one is passed on.

Tracks are named after their blocks, so **naming the blocks names the files**.
Two blocks of the same type with no names of their own become `Application` and
`Application 2`, in list order. If the template does not mention `{block}`,
` - {block}` is appended to it, because otherwise every track would resolve to
the same name and be told apart only by the order the files happened to open
in.

A block added to a running session gets a track from that moment, and one
removed closes its file - the mixed recording carries on either way, which is
the reason the per-block files hang off the input blocks rather than off the
recorder.

With two Recorder blocks both asking for tracks, an input block's file belongs
to the **first** of them below it. One input cannot be written to two track
files at once, so the second recorder gets its mixed file and nothing else.

### Rate and bit depth

A recorder block writes at the session's own rate and 16 bits unless you say
otherwise, which is what it always did. **Sample rate** and **Bit depth** on
the block's parameters change that for that one file.

Both lists hold only what the chosen format can actually be written as here,
because that is not the same everywhere and not the same for each format:

* **WAV** takes any rate, and 16 bit, 24 bit or 32 bit float. At 32 bit the
  samples go to disk exactly as the mix holds them, with no conversion of any
  kind.
* **FLAC** is 16 or 24 bit, at 44100 and up - Windows' encoder has nothing
  below 44100, which is the reason this setting exists as much as any
  preference: a session running at 32 kHz could not record FLAC at all before,
  and now it records at 48.
* **MP3** goes up to 48000 and no further, and has no bit depth.
* **AAC** is 16 bit, up to 48000, and also offers 96000.
* **Ogg Vorbis** is fussier than the rate alone, and libvorbis is asked rather
  than assumed: it refuses 192 kbps at 22050 Hz, and refuses every bitrate
  above 48000. So the rates offered stop at 48000, and **the bitrate list
  shortens as the rate comes down**.
* **Opus** is a 48 kHz format. It resamples anything it is handed, so there is
  nothing for the setting to decide and the control is switched off rather
  than left there doing nothing.

A combination the encoder has nothing for is refused with the rates it does
have named, rather than quietly written as something else.

The conversion happens on the recorder's own writer thread, not on the audio
clock, so recording at a rate of its own costs the session no latency and
cannot cause a dropout. It asks the resampler for its best quality for the
same reason: nothing is waiting on it but the file.

A Buffer block's Save asks the same questions with the same controls, because
it is the same question.

**Tracks inherit it.** A recorder writing a file per input block writes every
one of them at the rate and depth the recorder is set to, so they still line
up with each other and with the mixed file.

### Skipping silence

**Skip silence** stops writing while the audio is quieter than the threshold,
so the file contains what was actually happening and not the hours around it.
It is a gate, not a mute: quiet passages are absent from the file rather than
present as silence, which means a recording made this way is not a timeline.

**Keep going for** can be set all the way to **always**, which stops the gate
ever shutting again once it has opened. That turns the threshold into a "wait
until there is something to record" control: nothing is written until the first
sound, and everything after it is.

Two things stop it chopping the audio up. The **level** is measured over 20 ms
blocks rather than per sample, so one stray sample cannot open or shut it. And
the 300 ms before it opens is held back and written when it does, so the sound
that opened the gate is in the file instead of being clipped off the front of
it. **Keep going for** then holds the gate open after things go quiet, which is
what stops a pause mid-sentence starting a new hole.

While the gate is shut the session window says so, because there is no level
meter to look at and a recorder that is deliberately writing nothing should not
look like a recorder that is broken.

### Splitting

**Start new file** rotates the recording after a number of minutes, or after a
number of megabytes.

Time is counted in recorded audio, not wall clock, so it agrees with the length
of the file it produced - with the gate on, an hour of mostly-silence is not an
hour of recording. The boundary is exact: audio is written in quarter-second
slices and the split is taken wherever it falls, rather than after whichever
buffer happened to cross it. Nothing is lost across a join.

Size is measured on the file. A WAV is 16 bit PCM at a known rate, so its size
follows from the frame count exactly; an encoded file has to be asked of the
file system, and can overshoot slightly because an encoder decides for itself
how many bytes a second of audio becomes.

Whatever the setting says, a WAV is always rotated before 4 GB. RIFF stores its
sizes in 32 bits, so that is as large a file as the format can describe, and
the alternative is one that silently stops being readable at the end.

## Cut, copy and paste blocks

In the block list: **Ctrl+X** cuts, **Ctrl+C** copies, **Ctrl+V** pastes below
the selected block and **Ctrl+Shift+V** pastes above it. All four are on the
Block menu and the list's context menu as well.

Two paste commands rather than one with a setting, because two is already
everything: pasting *above the first* block is the start of the list and
pasting *below the last* is the end, so there is nowhere a block can go that
these do not reach. With nothing selected a paste goes on the end, which is
where a block added from the library goes too.

The clipboard holds **the block**, not a description of it - the plugin it is,
the parameter values it is set to, its EQ bands, its four action hotkeys, all
of it. It is FastRoute's own rather than Windows', deliberately: a cut is
destructive, so what comes back has to be exactly what went in, and a round
trip through text could lose a corner of a block without saying so. Sharing a
block with somebody else is what exporting a session is for.

It is process-wide, so a block cut in one session window pastes into another.

**Hotkeys are the one thing a paste can change.** A block pasted with its keys
intact into a session where something else already answers to them would make
a clash that nothing on screen explains, so those keys are cleared and the
status line says how many. That one rule gives both commands the behaviour
they should have without either knowing which it is: a cut block's original is
gone by the time it is pasted, so it keeps its keys; a copied one meets itself
and loses them. Moving a block to another session keeps its keys unless that
session is already using them.

Every one of the four says what it did in the status line - *Microphone copied*,
*Microphone pasted at position 2 of 5* - because there is nothing else on
screen that would say so.

Ctrl+C in the times field still copies the text. The clipboard keys mean a
block only when the block list has the focus; the menu items always mean the
block, since that is what they say.

## Pipeline semantics

The block list is walked top to bottom over a single bus:

* an **input** block mixes its volume-scaled capture into the bus,
* an **effect** block transforms whatever is currently on the bus,
* an **output** block renders the current bus contents to its endpoint.

That is what lets the three kinds interleave in any order. Putting an output
block above a second input means the first output only carries the blocks above
it, so one session can feed two destinations with different content:

    Application: game.exe        -> bus = game
    Gain 80%                     -> bus = game * 0.8
    Output Device: Headphones    -> headphones get game * 0.8
    Input Device: Microphone     -> bus = game * 0.8 + mic
    Output Device: Virtual Cable -> the cable gets game + mic

## Keyboard

### Sessions window

| Key            | Action                                    |
|----------------|-------------------------------------------|
| Enter          | Open the focused session                  |
| Ctrl+R         | Run the focused session, or stop it       |
| Ctrl+N         | New session                               |
| Ctrl+H         | Hide to the notification area             |
| Ctrl+,         | Settings                                  |
| Ctrl+Shift+N   | New scheduled event                       |
| Ctrl+Shift+O   | Open the session's recording folder       |
| Menu           | Run at Startup, on the Session and context menus |
| F2             | Rename                                    |
| Delete         | Delete                                    |
| Shift+F10      | Context menu, including Hotkey            |

### Session window

| Key            | Action                                              |
|----------------|-----------------------------------------------------|
| Enter (tree)   | Add the selected library block to the session       |
| Enter (list)   | Edit the focused block's parameters                 |
| Space          | Enable or disable the focused block                 |
| Delete         | Remove the focused block                            |
| Ctrl+Up/Down   | Move the focused block up or down the pipeline      |
| Alt+Up/Down    | Raise or lower the focused block's volume by 5%     |
| Ctrl+X         | Cut the focused block                               |
| Ctrl+C         | Copy the focused block                              |
| Ctrl+V         | Paste below the focused block                       |
| Ctrl+Shift+V   | Paste above the focused block                       |
| Ctrl+R         | Run this session, or stop it                        |
| Ctrl+H         | Hide to the notification area                       |
| Ctrl+,         | Settings                                            |
| Ctrl+Shift+O   | Open the recording folder                           |
| Esc            | Close the session window                            |
| Shift+F10      | Context menu, including Hotkey                      |

### Sliders, in any block's parameters

| Key            | Action                                              |
|----------------|-----------------------------------------------------|
| Up / Right     | Raise by one step                                   |
| Down / Left    | Lower by one step                                   |
| Page Up        | Raise by a page                                     |
| Page Down      | Lower by a page                                     |
| Home / End     | The minimum, the maximum                            |
| Alt+A          | Apply, without closing the dialog                   |

A trackbar left to itself takes its directions from its geometry rather than
from what the number means: on a horizontal one, Up and Left both step towards
the minimum. Left and right are right that way round, but up and down are not -
nobody turns a volume up by pressing Down, and driving a slider blind, one
arrow at a time with the reader saying the new number after each, a control
that disagrees with the word for it is worse than merely surprising. So every
slider in FastRoute has Up and Down the other way round from the stock control,
and Page Up and Page Down with them, so that every key that moves a value moves
it the way its name says. This matches Alt+Up and Alt+Down on a block in the
session window, which have always raised and lowered its volume.

#### What a slider says its value is

A trackbar reports its position as a percentage of its range, and a percentage
of a range is not a parameter. The parametric frequency slider is 200
logarithmic steps wide, so 250 Hz sits at step 137 and the control calls that
68. A pan hard left calls itself 0 and hard right 100, whichever way round the
sides are. A gain in dB calls itself whatever fraction of its own ends it
happens to be at. None of those numbers appears anywhere in the dialog, and
none of them is what the label beside the slider says.

That label is not what gets read when an arrow key moves the thumb, either.
The reader reads the control that has focus, and the control was saying where
the thumb is.

So every slider is annotated with the value written beside it, through MSAA
dynamic annotation - `IAccPropServices::SetHwndPropStr` on `PROPID_ACC_VALUE`,
which puts a string of ours in front of the value the control would otherwise
report, without subclassing it or replacing its accessibility object. The
sliders now say "250 Hz", "hard left", "-6 dB", "2 minutes", "any clipping",
"+3 dB, worked out" - the same words as the label, because they are set from
the same string in the same call, in `SetSliderReadout`. There is nowhere left
for the two to drift apart.

For a plugin, the words are the plugin's own: the slider reports whatever
`ParamText` gives back for the parameter, "-6.0 dB" or "Low pass", rather than
its position along the thousand steps the value is carried on.

This reaches a UIA-first reader as well as an MSAA one. The built-in proxy
takes the annotated value as the element's `Value.Value`, which is checked
rather than assumed. Its `RangeValue.Value` still carries the raw percentage,
which is correct - that is what the range genuinely is.

Nothing raises an extra value-change event. The trackbar already raises
`EVENT_OBJECT_VALUECHANGE` when the thumb moves, and the annotation is
rewritten inside the `WM_HSCROLL` the trackbar sends while it is still handling
the key, so a reader that takes that event and comes back to ask finds the new
value waiting for it. A second event would only make the value be read twice.
The annotation is given back when the slider is destroyed, because it is
remembered against the window handle and handles are reused.

All commands are also on the menu bar and on the context menus.

## Audio engine

Everything runs in-process on WASAPI; no driver or third-party router is
involved.

* Application blocks use the process loopback API
  (`ActivateAudioInterfaceAsync` with `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`),
  which needs Windows 10 2004 or newer.
* Device blocks open shared-mode clients with `AUTOCONVERTPCM`, so Windows
  handles sample-rate, channel and bit-depth conversion into the internal
  format: stereo float32 at the master output's rate. Bit depth therefore
  never reaches the mix at all, and a session can mix a 16-bit microphone with
  a 24-bit interface without being told about it.
* **The first output block owns the clock.** Its renderer calls the mixer
  directly from its own WASAPI event, so the graph runs at exactly the rate the
  sound card consumes and nothing is buffered between the mix and the endpoint.
  Any further output block is fed from a ring buffer by the master.
* **The pipeline runs at the master output's own sample rate**, taken from that
  endpoint's mix format, so that endpoint needs no conversion.
* **The output buffer is deliberately not kept full.** WASAPI hands out a
  buffer well over one engine period (1056 frames against a 480 frame period is
  typical); topping it up every event would make all of it latency. It is held
  at one period plus half instead, and each pull is capped at one period,
  because whatever the mixer pulls in one go has to be sitting in the capture
  rings first.
* **Capture rings are flushed on the first pull.** Opening the output client
  takes milliseconds during which the already-running captures bank audio.
  Left alone that startup backlog sits in the FIFO for the rest of the session
  as pure latency, so it is dropped at the moment the mixer actually starts
  consuming - not before, or the rings just refill over the gap.
* **An application block captures the target's whole process tree, always.**
  The process loopback API has no "this process only" mode: its two options are
  include-the-tree and exclude-the-tree, and the second one is not "without the
  children", it is the *inverse* capture - every process on the system except
  that tree. For a router that is also rendering, that quietly includes its own
  output, and the session feeds back into itself. There is no checkbox for it
  because there is no sane setting to offer.
* **Neither renderers nor captures use the `IAudioClient3` minimum-period path.**
  On real hardware it gains nothing: the engine periodicity is already held at
  the default by whatever else is playing, so the "minimum" comes back as the
  default anyway. On a virtual endpoint with no other users it is granted
  literally - a 1 ms period behind a 2.2 ms buffer - which runs the entire
  graph, resamplers and effects included, every millisecond with no headroom.
  The plain path returns the same 22 ms buffer on every endpoint here, virtual
  or not.
* **The old capture note:** On at
  least one endpoint here it returned 1 ms packets at about 1.22x real time,
  which overran the ring and shredded the stream. The plain shared-mode path
  with `AUTOCONVERTPCM` asks Windows' own resampler for our exact format, which
  pins the rate. The few milliseconds a shorter period would have saved are not
  worth that.
* **Capture rings resample rather than discard.** A capture device never runs
  at exactly the rate the output consumes at. Usually that is a fraction of a
  percent of clock drift; a device can also simply be wrong about its rate, and
  one endpoint here delivers 1.22x real time. Either way, reading out through
  an asynchronous sample rate converter holds the backlog steady without any
  audio being thrown away to keep it there. The ratio is measured directly from
  frames in against frames out over a window, with no clock involved, and
  trimmed by a slow term that holds the backlog at its working point. It has to
  be a ratio of *totals*: averaging per-read ratios while skipping the reads
  where no packet happened to arrive biases the estimate upwards by exactly the
  reads it discarded, and the converter then pitch shifts the whole session by
  several percent, wandering as the estimate wanders. The applied ratio is also
  clamped to five percent, because a device claiming 48 kHz is not really
  running further out than that, and resampling that far is worse than the
  drift it corrects.
* **The converter is latched off until an input proves it needs one.** At a
  ratio of exactly 1.0 with no accumulated fraction the interpolator (Catmull-
  Rom) is bit-for-bit passthrough, so a well-behaved input is left completely
  alone. Letting the ratio wander off unity on a healthy stream buys nothing
  and gives the loop something to hunt: consume slightly fast, starve, correct,
  overshoot. It engages only after a stream has genuinely broken continuity
  several times while measuring well away from our rate, and then stays on.

Measured on the development machine: **35 ms steady, zero dropouts**, over
repeated one-minute soaks with an application block, a microphone block, a gain
block and the default output all running at once, including block reordering
and enable/disable while playing.

The input side deliberately carries a 20 ms cushion. A 10 ms one measured
25 ms end to end and mostly held, but broke up intermittently - and once a
capture starves, the correction loop has no good options. The extra 10 ms is
bought margin, not waste.

The session window reports the live figure, split into what the output endpoint
holds and how deep the capture rings were at the last read. It also reports a
dropout count whenever the stream has actually been discontinuous - discarded
at a ring ceiling, or zero-filled because a capture ran dry. Anything other
than zero there is audible, so it is worth watching if a device misbehaves.

If the output endpoint ever runs dry, the fill target widens by a quarter
period and settles wherever the machine can actually sustain it, so an
aggressive default degrades into a working one rather than a glitching one.

## Editing a running session

Everything is live. Blocks can be added, removed, reordered, retargeted to a
different device or application, re-levelled and enabled or disabled while the
session is playing.

Applying an edit rebuilds the graph in place: blocks whose identity has not
changed keep their existing capture or render stream, so reordering or
adjusting one block does not interrupt any of the others. Only a block that has
actually been pointed somewhere new gets a new stream.

If an edit cannot be applied - say an application block naming a process that
is not running - the session keeps playing its previous configuration and the
error explains why.

**Which output holds the clock** is the part of that rebuild with teeth in it.
The first output block in the list drives the graph: its render callback is
what pulls every capture and runs every effect, and the other outputs are fed
through rings. Reordering the blocks can therefore hand the clock to a
different device, and the incoming master is very often a renderer that is
already running as a secondary.

That case used to kill the process outright. The rebuild stopped the *old*
master and deliberately skipped the new one, then started the new one - and
starting a renderer assigned to its `std::thread` member unconditionally.
Assigning to a joinable `std::thread` is `std::terminate()`: no error, no
dialog, no log entry, the process simply vanishes. From the outside it looked
like "I moved a block and it crashed", which is exactly what it was.

It is fixed in both places, deliberately. The rebuild now stops the incoming
master before promoting it, which is the honest expression of what has to
happen; and `DeviceRenderer::Start` stops itself if it is already running, so
no future caller can reach the same cliff by a different route.

## Running at startup

Any session can be marked **Run at Startup**, from the Session menu or the
context menu in either window. It is a checkable item, so a screen reader
reads back which way it is set, and the sessions list carries a Startup column
saying the same thing. The flag is per session and lives in `sessions.json`
with the rest of that session.

FastRoute starts every marked session once the main window is up. A session that
cannot start - most often because the application it captures has not launched
yet - goes to the error log rather than a message box. FastRoute can easily come
up before the applications it captures, and a dialog to dismiss at every login
is the wrong way to say so.

This does not make FastRoute itself launch with Windows. Put a shortcut to it in
`shell:startup` for that; the two are deliberately separate, so marking a
session does not quietly change when the application runs.

## Scheduler

The main window's second tab is a list of scheduled events. Each one starts a
session at a time, and can stop it again - at another time of day, or after a
number of minutes.

* **When**: a time of day, or a number of minutes from now. The second is for
  "in an hour"; it is worked out into a real date and time when you press OK,
  so it means the same thing tomorrow as it did while the dialog was open.
* **Repeat**: once, every day, or on chosen days of the week.
* **Stop**: leave it running, stop at a time of day, or stop after so many
  minutes. A stop time at or before the start means the following day, which is
  how an overnight run is written.
* **Remove once it has run** deletes the event afterwards, for a reminder
  rather than a routine. The stop still happens: it is remembered separately
  from the event, so a single-use event can be deleted the moment it fires and
  still switch its session off later.

Events are checked a few times a minute rather than on an exact alarm. A router
starting a second late is nobody's problem, and an alarm that has to survive
sleep, clock changes and the machine being off is a great deal of machinery for
that second.

**An occurrence that has been and gone is not run late.** If FastRoute was not
running at nine in the morning, starting the nine o'clock session when it opens
at three in the afternoon is not what anyone meant. Anything more than five
minutes overdue is left alone and noted in the error log instead.

## Where settings live

Beside the executable, always. Copy the folder to a memory stick and the
sessions, the schedule and the settings go with it rather than staying behind
on whichever machine it was plugged into. The cost is that a copy put somewhere
you cannot write to - Program Files, most obviously - has nowhere to save.

A version before this kept it in `%APPDATA%\FastRoute`. If there is nothing
beside the executable yet, that is read once and written back out beside the
executable on the next save, so moving to a portable build does not look like
every session having been deleted. The old copy is read, never written, and
left alone.

Sessions are kept in name order, sorted the way a reader would sort them rather
than the way the code points fall. The list and the document are the same
order, so no command can disagree with what is on screen about which row is
which.

## Windows versions

FastRoute runs on Windows 7 SP1 and later. Everything newer than Windows 7 is
resolved at run time, because a single import a release does not have stops the
process from starting at all - not a missing feature, a missing application.
The CRT is linked statically, so there is no redistributable to install.

Two things are therefore conditional:

* **The Application input block** needs process loopback capture, which arrived
  in Windows 10 version 2004 (build 19041). That applies whether it is pointed
  at a name or at the foreground. Below that the block is left out of
  the library tree entirely rather than offered and then refused. A session
  saved on a newer machine still opens, and says exactly what is missing when
  you run it. Capture an input device instead, or route the application into
  one with a virtual cable.
* **Excluding applications from System Audio** needs the same thing. The block
  itself works everywhere: below 2004 it captures the output device whole, and
  the exclusion list is disabled with a note saying why.
* **FLAC, MP3 and AAC recording** use the encoders Windows ships, so which of
  them exist depends on the release. Formats this machine cannot encode are
  left out of the recorder block's format list. WAV, Ogg Vorbis and Opus are
  written by FastRoute itself and always work, including on editions with no
  Media Foundation at all - those DLLs are delay loaded and are not touched
  until something has confirmed they are there.
* **Streams** work either way round: Ogg Vorbis and Opus are decoded by
  FastRoute, everything else by Windows. On an edition with no Media Foundation
  the stream block still plays Ogg and Opus.

The version is detected with `RtlGetVersion`. `GetVersionEx` reports 6.2 to
anything without a `supportedOS` manifest entry, which would make every
Windows 10 look like Windows 8.

## Hiding to the notification area

**Ctrl+H**, or **File > Hide to Tray**, puts FastRoute in the notification area
and takes its windows off screen. Sessions keep running: hiding is about the
window, not the audio, and the tray icon's tooltip says how many sessions are
going so a router you cannot see still tells you what it is doing.

**Ctrl+Alt+R** brings it back, and hides it again - it toggles. That one is a
global hotkey, registered with Windows, so it works from whatever you are using
at the time. It has to be: when the window is gone there is nothing to press a
key in. Change it under **Ctrl+,** (Settings), where it is four checkboxes and
a list of keys rather than a "press the combination" box, so a screen reader
reads it back correctly and there is no moment where you have to press a key to
find out what it does.

A hotkey another program already owns cannot be registered. Settings tries the
new one before keeping it and puts the old one back if it fails, rather than
saving a hotkey that silently does nothing. A hotkey with no modifier is
refused outright: it would take that key away from every other program running.

FastRoute only hides when asked. **Closing the window still exits and
minimising still minimises** - a window that vanished on Alt+F4 would be a
window people could not get rid of. The tray icon exists only while hidden, so
the notification area is not carrying an icon for a window that is on screen
anyway. Its menu offers Show, Settings and Exit; Enter or a click on the icon
shows the window.

## Hotkeys for one session or one block

Shift+F10 on a session in the sessions list, or on a block in a session's block
list, offers **Hotkey**. A session's hotkey starts and stops it; a block's
switches that block on and off.

Each one is either **global** or **window**:

* A **global** key is taken from the whole system with `RegisterHotKey`, so it
  works from inside whatever you are actually using - which is the point,
  because a router is something you reach for while doing something else. The
  cost is that the key is gone from every other program for as long as
  FastRoute is running, which is a real price for a common combination.
* A **window** key is only read while a FastRoute window is in front. It is
  never registered with the system, so it cannot fail, cannot clash with
  another program, and costs that program nothing. It is the right choice for
  the keys you use while looking at FastRoute anyway, and it means the good
  combinations do not have to be spent.

Global is the default, and a hotkey saved before this choice existed reads back
as global, so nothing changes meaning underneath an existing document.

**The key is named, not chosen from a list.** You type what the key is called -
`Left Arrow`, `F5`, `Num 7`, `Page Up`, `Media Play` - into an ordinary text
field. It used to be a list of letters, digits and function keys, which quietly
ruled out every arrow, Home and End, the numeric keypad and the media keys, all
of them perfectly good hotkeys and several of them the obvious choice for a
transport control. A name has no list to be left off.

Typing the name rather than pressing the key is deliberate. A field that
captured keystrokes would have to swallow Tab to let you bind it, and a field
you cannot leave without a mouse is no use here. It would also be unable to
accept any combination the shell takes first, which is most of the Windows key
ones.

The reading is forgiving: case, spaces and the word "arrow" are all optional,
so `Left`, `left arrow` and `LeftArrow` are the same key, and the common short
forms - `esc`, `del`, `ins`, `pgup`, `pgdn`, `numpad7` - are read too. What the
field is filled with is always something it will accept again, so opening a
hotkey and pressing OK without touching anything cannot change it.

A name that is not a key is refused on the way out rather than quietly leaving
the hotkey unset, and the note under the field says what went wrong while you
are still typing.

A modifier is still required. A bare Left Arrow as a global hotkey would take
the arrow key away from every other program on the machine, and as a window key
it would break moving around the very lists it is set from.

Window keys are matched in the message loop before the dialog manager sees the
key, which is what lets them beat a control's own handling. The exception is
text editing: while the caret is in an edit field, Ctrl+A, C, V, X, Z and Y and
Shift+Insert and Shift+Delete keep meaning what they mean everywhere else. A
router that ate Ctrl+V in its own file name field would be its own bug report.

Holding a window key down acts once rather than repeating, the same as the
`MOD_NOREPEAT` the global ones are registered with.

Both lists show the scope in the Hotkey column - `Ctrl+R` against `Ctrl+R (this
window)` - because two keys that read alike but work in different places are
otherwise impossible to tell apart.

Each one answers with a tone: **800 Hz for on, 400 Hz for off**, 50 ms. When
the key was pressed in another application, with FastRoute possibly hidden
altogether, that tone is the only confirmation there is. It plays only for
hotkeys - toggling the same thing in the window does not beep, because you can
already see what happened.

The hotkeys are re-registered from scratch every time the document is saved, so
a binding never ends up pointing at a block that has since moved or been
deleted. Both lists carry a Hotkey column, so what is bound to what is readable
without opening anything. A hotkey another program already holds cannot be
registered; those are collected into one entry in the error log rather than a
message per key. Two window keys set to the same combination are reported the
same way: only the first would ever fire, and the second silently doing nothing
is worse than being told.

### One key, every session

A **block action's** hotkey - rewind, forward, back to live, save the buffer -
is registered only while its session is running. There was never anything for
it to do while the session was stopped: an action is done to a running graph,
so it answered with the low tone and changed nothing. Leaving it unregistered
the rest of the time has two consequences worth having:

* the same key can mean *save the buffer* in every session you have, as long as
  only one of them is running at a time; and
* a key you only use while recording is not taken from the rest of the machine
  while you are not recording.

Two *running* sessions claiming one key are still a clash, and reported as one.
A key cannot mean two things at once.

A block's own on/off hotkey is deliberately **not** treated this way. Switching
a block off is an edit to the document rather than something done to a graph, so
it means the same thing whether or not the session is running and it stays
registered either way. That division was Gavin Grundlingh's suggestion.

The hotkey window says which of the three kinds you are setting and what it
will therefore do, because the fields are identical for all three and the
sentence underneath is the only thing that tells them apart.

The table is rebuilt whenever a session starts or stops as well as on every
save, and the clash report goes to the error log only when it *changes* - a key
another program holds for good would otherwise be reported on every run.

A session started from a hotkey that fails - because the application it
captures is not running, say - reports to the error log and answers with the
low tone, rather than putting a modal box in front of whatever you were doing.

## Error log

**Help > Error Log** shows every error FastRoute has reported since it started,
oldest first, each stamped with the time. Anything that raises an error box is
recorded here as well, so a message that appeared while you were reading
something else is still there afterwards, and so are the failures that are
deliberately never shown as a box - a session that could not start at startup,
for instance.

The entries sit in a read-only multiline edit field: an error is often a
paragraph, and an edit control wraps it, lets it be selected, and lets a screen
reader walk it by line, word or character like any other text. **Copy** puts the
whole log on the clipboard and **Clear** empties it. The log holds the most
recent 200 entries and is not written to disk.

## Packaging

`dist.ps1` builds and packages a zip into `dist\`:

    .\dist.ps1                     # Release, named by today's date
    .\dist.ps1 -Version 1.0
    .\dist.ps1 -SkipBuild          # package what is already built

It collects `FastRoute.exe`, any DLL that ended up beside it, and everything in
`docs\`. The application icon is built into the exe (`src/fastroute.ico`), so
there is nothing else to ship. This README is not included: it is about building FastRoute, which is
not what someone unpacking a zip is holding.

## Storage

Sessions live in `%APPDATA%\FastRoute\sessions.json`, written through a temporary
file and swapped into place so an interrupted write cannot truncate the
document.

## Third party code

Nine libraries are fetched at configure time and built from source, pinned to
an exact version. All are BSD or MIT licensed; `docs/Third-party notices.txt`
carries the notices that go out with a build.

| Library | What it is for |
|---------|----------------|
| RNNoise | the RNNoise Denoise block |
| SpeexDSP | the Speex Denoise block, and rate conversion for streams and Opus |
| Signalsmith Stretch | the Pitch Shifter block |
| libFLAC | FLAC over the network, for the Net Send and Net Receive blocks |
| libogg | Ogg framing, both directions |
| libvorbis, vorbisfile | Ogg Vorbis, decoded for streams and encoded for recordings |
| libopus, opusfile | Opus, decoded for streams and encoded for recordings |
| CLAP | the CLAP plugin interface, for hosting external effects |
| VST3 `pluginterfaces` | the VST3 plugin interface, likewise |

libFLAC is only needed because Media Foundation's FLAC support writes a file.
That is all a Recorder block wants, and no use at all for putting one frame in
each packet, which is what the net blocks do. Only the C library is built: the
C++ wrapper is a convenience for file work and the Ogg framing is for files
too. FLAC links `Ogg::ogg` whenever that target merely exists, which it does
here because libvorbis and opusfile both wanted it; the link is harmless, since
none of the Ogg framing is compiled, but FLAC's install rules then try to
export a target belonging to somebody else, so those are turned off - nothing
here is ever installed.

Signalsmith Stretch is pinned at 1.1.0 rather than at its main branch, because
1.1.0 is self-contained: one header and the `dsp/` headers beside it. Later
versions moved the FFTs out into a separate Signalsmith Linear repository. The
repository's one submodule belongs to its command line tool and is not fetched.

VST3 became MIT licensed with version 3.8 in October 2025. Before that it was
GPLv3 or a proprietary agreement, which made hosting it in a project like this
one a licensing decision rather than a technical one. Only `pluginterfaces` is
taken: the rest of the VST3 SDK is for writing plugins, and its editor library
is of no use to a host.

Everything else - WASAPI, Media Foundation, WinHTTP, the common controls -
ships with Windows.

## Verifying audio quality

Latency counters and dropout counts say nothing about whether the audio is
*correct*. Three separate faults here - an inverse process-loopback capture, a
1 ms engine period on virtual endpoints, and a biased rate estimator - all
produced healthy looking numbers while mangling the signal.

The check that catches them is a known signal end to end: play a 1 kHz sine,
route it, capture the destination endpoint with WASAPI loopback, and find the
spectral peak. Correct output puts the peak at 1000 Hz with the expected
magnitude. A pitch shifted peak means a resampler is engaged when it should not
be; a collapsed magnitude with the RMS intact means the shift is wandering; an
RMS above the source means something is being summed in twice.

Current measurements, application block to output device, source magnitude
5000 at 1000 Hz:

| Destination                | Peak     | Magnitude |
|----------------------------|----------|-----------|
| Recorder (the bus itself)  | 1000 Hz  | 5000      |
| Virtual Audio Cable output | 1000 Hz  | 4999      |
| Realtek speakers, 25% vol  | 1000 Hz  | 1250      |

The codecs are checked by going round the loop: a 1 kHz tone is recorded in
each of the six formats, each file is decoded again, and the tone has to come
back at 1000 Hz and the right length. Then the same files are served over HTTP
and played through the stream block, which has to produce 1000 Hz with no
dropouts. Ogg and Opus are read back by the same libraries that wrote them, so
the recorder and the stream block check each other.

The renderer promotion that used to crash is checked by doing it: a renderer
is started the way a secondary output starts, then started again the way the
rebuild promotes it to master. Before the fix that check did not fail, it
killed its own process with `0xC0000409` before a single line of output had
been flushed - which is the clearest possible statement of what the bug did.
That harness opens a real output device, so it is not part of the routine run.

The default recording folder, the hotkey scopes and the endpoint scoping are
checked offline: no audio device is opened, no window is created and nothing is
brought to the foreground. The document round trips are safe to run because
`ConfigDir()` is beside the executable and a harness executable does not live
beside the real one - it writes its own `sessions.json` next to itself and
never sees the user's.

The hotkey checks are mostly about the past rather than the present: that a
`hotkey` object with no `global` member reads back as global, so a document
written before the choice existed does not quietly change what its keys do.

The endpoint scoping is checked by listing the audio sessions on each render
endpoint and comparing that with the machine wide list - read only, the same
information the volume mixer shows. On the machine this was written on, one
virtual cable had three processes on it against ten machine wide; the seven it
no longer captures included a mail client, a chat program and the screen
reader, none of which had ever been part of that device's mix.

The buffer is checked by feeding it a ramp that never repeats, so a sample's
value says exactly which frame it came from and "am I ten seconds back" becomes
a thing that can be measured rather than judged. At live every frame comes
straight back out; one rewind lands ten seconds back to within a tenth of a
second; a second one lands twenty; fast forward comes back a step and jump to
live returns to live. Rewinding further than it has kept is clamped to
something it has actually recorded, fast forward at live does nothing, an
action the block does not have is ignored, and changing the length puts it back
to live rather than leaving a stale read point in a freshly allocated ring.
Saving is checked by doing it, into the scratchpad rather than any folder of
the user's, with the audio still running throughout: the file that appears has
to hold about what went in - four seconds of it, not a bare header and not the
whole unwritten ring.

Naming a key is checked against a table of what each one should read as -
"Left Arrow" rather than "Left", which read aloud is ambiguous with the Left in
Left Shift - plus two properties that matter more than any single name: that
every key code from 1 to 255 has some name at all, and that no two keys worth
binding read as the same thing, since a list of hotkeys that cannot tell two
keys apart is worse than no list.

Reading a name back is checked the same way, in both directions: that every
spelling worth typing lands on the right key, that a name which is not a key
resolves to nothing rather than to something near it, and - the one that would
bite hardest if it broke - that every key this writes down it can read again.
The field is filled from the document by naming the key and read by parsing
that name, so if those two ever disagreed, opening a hotkey and pressing OK
without touching anything would silently change it.

The action hotkeys are checked through the document: that a window scoped key
and a global one on the same block both come back with their scope intact, and
that the actions with no key set stay unset rather than arriving as something.

The normaliser is checked by measuring it: a steady tone at a known level in,
and the level out compared with the target. -20 dB in against a -3 dB target
comes out at -3.00; 0 dB in against -6 comes out at -6.00; -40 dB in with the
boost capped at 10 dB stops at -30.00 rather than reaching the target, which is
the cap doing its job. Silence stays silent, the reported latency is the
lookahead, a document full of out of range values is clamped rather than
obeyed, and wound fully dry the output is the input delayed and otherwise
untouched, sample for sample.

The clipping monitor is checked for the one property that matters and can be
checked in silence: that every sample comes back exactly as it arrived. Its
beep is deliberately never triggered by a test. It is audible by design, and a
test suite that makes noise on the machine it runs on is a bad test suite - so
the checks feed it a signal well under its threshold and the beeping path is
left to be tried by hand.

The plugin hosts are checked against whatever CLAP and VST3 plugins are
actually installed, because none of it can be faked: whether a module loads,
whether it says it is an effect, whether it will run at 48 kHz and hand back
finite numbers. Each one is loaded, started, asked for its parameters, fed
silence and then a tone in the odd sized chunks the engine really uses, and
checked for NaN - a plugin that returns one poisons the whole bus for as long
as it is in it. A parameter is set and read back, and the plugin is asked to
describe a value. Then the same thing again through the effect block, which is
the path a running session takes, plus a block whose plugin has been
uninstalled, which has to leave the audio alone rather than stop the graph.
Separately, a live harness drives the real Settings dialog, presses the real
Scan button, waits for the report, and then reads the document to see what was
written - and runs a session with a plugin in it.

The noise gate is checked as a state machine rather than as a filter: that it
passes a loud signal untouched, cuts a quiet one by exactly the range asked
for, stays open part way through the hold and is shut once the hold and release
are done, and - the part that separates a usable gate from one that chatters -
that a signal sitting on the threshold, and one 2 dB below it, both stay open,
while one 5 dB below shuts.

Its lookahead is checked for the four things that could be wrong with it: that
the latency it reports is the latency it has, and is nothing when there is no
lookahead or the gate is cut by 0 dB; that a gate held open by a loud signal is
the input delayed by exactly that much and altered in no other way, sample for
sample; that a burst after silence starts at full level with the lookahead and
is still climbing without it; and that a quiet signal is still cut by the whole
range, since a lookahead that quietly held the gate open would be a gate that
had stopped gating. Every other check in that harness asks for no lookahead, so
what they measure is still the gate and not the delay line in front of it.

The running-time fields are checked live for what they claim to be: that each
is an edit control, read only, multiline, and actually refuses typing, and that
the clock in it advances while a session runs and a recording grows.

The foreground blocks are checked in two halves. The rule that decides whether
a window is an application worth switching to is a plain function of a window
class and an executable name, so it is tested directly against sixteen of them:
browsers and File Explorer count, the taskbar, the desktop, task view, Alt+Tab,
the Start menu, the search box, the touch keyboard and the lock screen do not.
The rest needs the real thing, so two harnesses drive the running application:
one opens Notepad, starts a following session from the FastRoute window, and
requires the status line to say it is capturing Notepad rather than FastRoute,
then brings FastRoute to the front (it must not move), then Paint (it must),
then Notepad again. The other does the same with a block set to decide once,
and requires the opposite: it must stay on Notepad when the foreground moves,
and move only when the block is switched off and on again.

The parametric EQ is checked shape by shape, because a filter that is the
wrong shape still sounds like something and passes any test that only asks
whether the audio changed. A peak has to reach its gain on the centre and be
out of the way two octaves either side; a high Q has to be measurably narrower
than a low one at the same gain; a shelf has to reach full lift below its
corner and be half way there at the corner itself; and the two pass filters
have to be 3 dB down at the corner and about 24 dB down two octaves past it.
The pass filters also have to work at 0 dB, where a peak is a straight wire -
that distinction is what the shared filter designer had to learn, and the ten
band EQ is re-checked in the same harness to prove teaching it did no harm.

The compressor and the limiter are checked with square waves and single
samples rather than music, because a square wave has a constant absolute value:
the detector sees one steady level, so the answer is the static curve exactly
and there is nothing to hide behind. The compressor is measured against the
arithmetic (-20 dB threshold at 4:1, 0 dBFS in, -15 dBFS out) at six points on
the curve, plus the knee, the stereo linking, the attack time constant and the
automatic makeup. The limiter is checked for the one promise it makes - nothing
above the ceiling, at four ceilings times four lookahead settings, including a
full scale single sample - and then for the things that promise could be met
dishonestly: the delay is exactly the lookahead, under the ceiling it is a pure
delay to the sample, and a steady tone comes out scaled rather than clipped.

Both of those caught something. The limiter's delay line was reading a slot
before writing it, which delays by the whole buffer rather than by the
lookahead - one frame too many, and it put the sample being output just outside
the window its peak was being looked for in. The ceiling clamp was hiding it.

The filter blocks are checked the same way but offline, against the real
effect objects rather than a copy of the maths: a sine at a known frequency in,
The two wire formats are checked against the specifications rather than against
themselves. A pack and a parse that are wrong in the same way agree with each
other perfectly, so the VBAN header is compared byte for byte with an array
written out by hand from VB-Audio's document - the rate index with the sub
protocol in its top three bits, the frame and channel counts each one less than
they are so that a byte can say 256, the data type with the codec in its top
four bits, sixteen bytes of name and a little endian counter - and only then
round tripped. The PING0 structure is checked to be 676 bytes, which matters
more than any field in it: a VBAN host only answers a request whose PING0 is
present, even when every field of it is zero.

The jitter buffer is checked by mistreating it on purpose. Packets are built
by hand rather than by a Net Send block, because the point is to control the
order and the gaps - a real sender will not drop packet twenty for you on
request - and sent to the receiver over a group with a hop limit of zero. The
measurement in every case is the **frame count**, because that is what the
clock rides on: a known number of frames goes into a network behaving badly and
exactly that many has to come out. One lost packet, four lost packets including
two together, pairs swapped, a dozen arriving backwards, packets arriving twice,
and a codec change mid-stream all come out at 8400 frames of 8400. Loss,
reordering and the worst hold time are checked to be reported rather than
hidden, since a link that is misbehaving is one somebody needs to be able to
diagnose.

Opus rebuilding a lost packet from its successor is checked to produce audio
rather than silence, and the codecs with no such mechanism are checked to say
so rather than to pretend.

The pitch is measured rather than listened for. A tone goes through a real Ring
driven exactly as the receiver drives one - whole packets in, device periods
out, with stalls and uneven arrival - and the phase of the output at the input's
own frequency is tracked across the run, so any drift in that phase is a
frequency error in cents. Two numbers come out and they had to be separated
before either meant anything: what the drift correction is *asking* for, which
is the only one that is pitch, and what a phase tracker *measures*, which also
picks up the splice every dropout leaves behind. Conflating them is what made
the first attempt at this look like a failure when the correction was already
behaving. The correction is checked never to ask for more than five cents in
any of six conditions, including a sender genuinely running 500 parts per
million fast and another the same amount slow - far more than two real crystals
differ by - and in those two the buffer is checked to hold its depth, since a
correction gentle enough to be inaudible still has to work.

ASIO is checked as far as it can be without hardware, which is not far: the
driver list is real, and enumeration is what the dialog opens with, so that
much is verified against the drivers on this machine. Everything past it -
opening a driver, its callback, the sample conversions - takes the audio
hardware away from whatever is using it, so none of it has been run here. It
is written to be read rather than to be trusted.

The updater is checked in three parts. The two decisions that can be wrong
without anybody noticing - reading the version and commit out of a release
body, and ordering two version strings - are checked without a network, against
the literal text the workflow writes rather than a convenient sample: a parser
that disagrees with it finds no version and then silently never offers an
update again. Then the check itself is run against the real release, and the
comparison is checked in both directions by compiling the harness as though it
were built from the release's own commit and then from a different one. Then
the download is run against the real asset and the file on disk is checked to
be exactly the size the release says, and to begin "PK" rather than being an
error page.

Applying an update is deliberately not checked, because doing so would replace
the copy of FastRoute on the machine running the check.

A receiver asking a sender for the audio is checked over a real socket, which
is the shape every link between two machines takes: a sender is started having
been told nothing about any receiver, a receiver is pointed at the machine
sending, and the tone arrives - at -6.021 dB, the level it went in at - with
the sender's own status line reporting "to 1 receiver that asked". A sender
asked for a stream it is not carrying is checked to stay quiet, since one
machine can run several senders on one port and the wrong one answering would
mix two streams together.

The two hop limits are checked to be separate, because they were not at first
and the failure was silent: a block sending to a named host inherited the limit
meant for keeping a group on the local subnet, and one hop does not survive the
first router, so the packets never left. The check sets the group limit to one
and then to zero and confirms the unicast TTL is still the system's own both
times.

Then the two blocks are pointed at each other and a tone is sent through them,
over all five codecs and over both protocols. Nothing leaves the machine: the
multicast senders are given a hop limit of zero, which means a packet is never
forwarded past the local host, and the VBAN test sends to 127.0.0.1. The tone
goes in at -6.021 dB and comes out at -6.021 dB on every lossless codec, with
the peak still exactly 0.5; Opus comes back within 0.02 dB. A VBAN stream with
the wrong name is checked to be left alone, since one VoiceMeeter sends several
to one port. Discovery is checked end to end: a running sender turns up in the
list under its own name, with its codec, its rate, its machine and where to go
and get it, and is found again by name the way a receiver follows a sender that
has been restarted. And every way a block can be unusable - no address, no
stream name, a group setting pointed at one machine, a VBAN sender at a rate
VBAN cannot describe - is checked to refuse with a reason rather than to fail
quietly.

The pitch shifter is checked against the two things that would make it useless
without being obvious. The first is that it lands where it was sent: a 220 Hz
tone is shifted by an octave each way, by a fifth, and by a hundred and by fifty
cents, and the result is measured with a Goertzel sweep rather than by
autocorrelation, which will report an octave down as readily as the right
answer. Where the shift is far enough for the two to be told apart, the old
pitch is checked to be at least 20 dB down as well, since a shifter that quietly
did nothing would pass the first measurement on its own.

The second is that it costs exactly what it says. Each of the three quality
settings is checked to report its own window - 120, 60 and 30 ms - and bypassed,
the block is checked to hand back the input delayed by exactly that many frames
and altered in no other way, sample for sample. At no shift it is checked to
report no latency and to be bit exact. It is also run three times over with the
same audio in one call, in 512 frame pieces and in 97 frame pieces, because the
engine hands over whatever the device asked for and the answer must not depend
on where those seams fall: all three come out bit identical.

What a slider tells a reader its value is gets checked through the
accessibility layer itself, not through the label. The harness makes a
trackbar, never shows it, and asks it the way a screen reader would: it
confirms first that a bare trackbar really does answer with a percentage - 68
for step 137 of 200, which is the whole problem stated as a number - then that
an annotated one answers with the parameter, through MSAA and through UIA's
proxy alike; that a value which is words rather than a number survives; that
the thumb has not moved and the slider is still named by the static in front of
it; that a moved slider reports the new value; that two sliders in one dialog
keep their own; and that the trackbar raises the value-change event by itself,
which is why FastRoute does not raise a second one.

The slider keys are checked by pressing them. The harness makes a trackbar,
never shows it, and posts the keys straight at it: that Up and Page Up raise
and Down and Page Down lower, that Left, Right, Home and End are untouched,
that a held key repeats, that the ends hold, that a second slider is found
without being named while an edit box beside it is left alone, and that
installing the swap twice is not one step up and one step down. It also asserts
what a bare trackbar does first, so that if Windows ever ships one that already
goes the right way, the check says so rather than the swap quietly undoing it.

measured gain out. Each of the ten EQ bands lands within 0.01 dB of its slider
on its own centre frequency; the tone shelves land within 0.2 dB of theirs; the
16 kHz band drops out at a 32 kHz rate; a flat EQ and a bypassed effect are
both bit exact passthroughs; and the channel mixer's width, reverse and centre
cancel are checked against hand computed sample values. That harness is how the
EQ's cascade overshoot was measured rather than guessed at.

RNNoise off 48 kHz has a harness of its own, which is about the conversion
rather than the model: that it starts at every rate an endpoint is likely to
be at, that it gives back exactly as many frames as it was given at each of
them, that the delay ten seconds in is the delay it started with, that a tone
comes out at the amplitude it went in from 100 Hz to 12 kHz, that silence
stays silent, and that the latency it reports is the latency a cross
correlation measures - to the sample. At 48 kHz it also checks that a dry pass
is still the input delayed, sample for sample, so the path that was already
there did not change.

The mixer's per channel volume and pan have a harness of their own, covering
the arithmetic and the document both: that the defaults are the identity
sample for sample, that a channel panned hard over leaves nothing at all
behind on its own side, that power stays flat across a pan sweep, that the
bottom of the volume slider is zero rather than nearly zero, that out of range
values from a hand edited document are clamped, and that a sessions.json
written before these controls existed loads back as the identity.

## Known limitations of this slice

* RNNoise below 48 kHz is the model working on converted audio, not on the
  audio as captured. The conversion measures clean - flat to a thousandth of a
  dB across the band it matters in - but the model was trained on 48 kHz
  material and what it hears at 44.1 kHz has been through two filters first.
  Whether that changes what it decides to remove has not been measured against
  real speech, only that the audio around it survives intact.
* RNNoise's measured effect on synthetic broadband noise is modest, around
  5-8 dB, and that is the model behaving as designed rather than a wiring
  problem: it is trained to lift speech out of noise, not to gate noise on its
  own, and its voice-activity output sits around 0.4 on material like that. It
  was verified against the library called directly on identical samples, which
  gives the same figure. Judging it properly needs real speech through real
  hardware. Speex measures about 18 dB on the same signal.
* Recordings are 16 bit. That is lossless for FLAC and WAV as far as the mixer
  is concerned only if nothing upstream exceeds full scale, since the bus is
  float and gets clamped on the way out.
* An application block cannot capture a process without also capturing its
  children, because the API does not offer that.
* Shared-mode WASAPI sets the latency floor: roughly one engine period held at
  the output plus the capture cushion. Going below needs exclusive mode, which
  would take the device away from every other application.
