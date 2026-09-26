# Instructions to run

How to get Kikkar running on the STM32N6570-DK. For what the project is and how
it works, see the [README](README.md).

## What you need

- STM32N6570-DK, connected by USB-C to its ST-LINK port
- An Ethernet cable to the same network as your computer
- macOS or Linux with Python 3.8+ and STM32CubeIDE 1.19+ (for its compiler and programmer)
- `pyserial`, only if you use `tools/run_on_target.py`

The microphone, display and Ethernet are all on the board. Nothing else needs
wiring.

## 1. Get the code

```sh
git clone https://github.com/SherTheCoder/Folklore.git
cd Folklore
```

Keep the folder path free of spaces; STM32CubeIDE's generated makefiles break
on them.

## 2. Download and convert the model

```sh
curl -L -o model/kikkar_kws_int8.tflite \
  https://huggingface.co/SherTheCoder/TeamFolklore_Model/resolve/main/kikkar_kws_int8.tflite
make model
```

`make model` checks the model against its architecture and converts it into the
flat format the firmware reads. You should see `23 ops in 5 stages` and
`31472 int8 weights, 2384 B of streaming state`. If there is no model in
`model/`, it builds an untrained placeholder instead, so everything still
compiles and the board tells you on startup. More in
[`model/README.md`](model/README.md).

## 3. Set the IP addresses

In [`Appli/Core/Inc/app/kikkar_config.h`](Appli/Core/Inc/app/kikkar_config.h),
set the board's address (`NET_LOCAL_IP0..3`) and the address of the computer
that will receive the audio (`NET_SERVER_IP0..3`, port `NET_SERVER_PORT`, 5555
by default). Keep both on the same subnet.

## 4. Build

```sh
make
```

The build ends with a size report and this line:

```
no collisions: the Appli can be downloaded while the FSBL runs
```

## 5. Start the receiver

On the computer at `NET_SERVER_IP`:

```sh
./server/asr_gateway.py --port 5555 --out captures/
```

Add `--asr 'whisper {} --model tiny.en'` to transcribe each command. 

## 6. Flash and run

The first-stage bootloader (FSBL) has to be on the board first. Import both
projects into STM32CubeIDE, build `KikkarProject_FSBL`, and program the
resulting `FSBL/Debug/KikkarProject_FSBL.elf` once, with the board in
development boot mode. After that, only the application changes.

Then use either:

- **STM32CubeIDE:** launch `Appli/KikkarProject_Appli.launch`
- **Command line:** `./tools/run_on_target.py --free`

`--free` detaches the debugger once the board is running. Use it when you want
to see the true colours: two of the display's red lines are shared with the
debug port, so colours look slightly off while a debugger is attached.

## 7. Check that it's listening

Open the serial port at **921600 baud** (`screen /dev/cu.usbmodem* 921600`).
The startup messages list the RAM budget, including the model:

```
  model              39120 B in flash, 23 ops, 31472 int8 weights, "Utho Kikkar"
  model arena         5264 B  3 activation slots + 2384 B of streaming state
```

and then:

```
Listening for "Utho Kikkar". Say it and the flower opens.
```

and then a status line every two seconds:

```
cpu=0.5% ... | link=3 tcp=2 up=8140ms | tx=0B ovh=0B rexmit=0 drop=0
```

`link` above 0 means Ethernet is up (3 is gigabit), and `tcp=2` means the board
is connected to the receiver.

## 8. Say the wake word

Say **"Utho Kikkar"** (उठो किक्कर or ਉੱਠੋ ਕਿੱਕਰ), then your command, in one go:

> Utho Kikkar, turn on the living-room lights.

The flower opens, and the receiver prints the command and saves it as a WAV:

```
START  utterance 1  keyword 'Utho Kikkar'  16000 Hz  16-bit  pre-roll 5120 samples
END    utterance 1  38720 samples  121 chunks  reason 0
       wrote captures/utterance-0001.wav
```

About 800 ms after you stop talking, the flower starts to close and the board
goes back to listening. The connection stays open for the next command.

## If something goes wrong

| What you see | What to do |
|---|---|
| `model not loaded` at startup | Redo step 2; the model's step or mel bands don't match `kikkar_config.h` |
| `untrained placeholder` at startup | There was no `.tflite` in `model/` when you ran `make model`; redo step 2 |
| `link=0` | Check the Ethernet cable |
| `tcp=0` | The receiver isn't reachable. Check both IP addresses and that `asr_gateway.py` is running |
| Flower never opens | Speak closer to the board, or lower `KWS_THRESHOLD` in `kikkar_config.h` |
| Flower opens on its own | Raise `KWS_THRESHOLD`; the value belongs to the int8 model, so pick it on that |
| Colours look wrong | A debugger is attached; run with `--free` |
| `drop=` above 0 | The network can't keep up and audio is being dropped |
| Board stops responding during download | Run `make mapcheck` |

## Useful commands

```
make            build the firmware
make model      convert the model in model/
make size       show flash and RAM use
make mapcheck   check the memory map against the FSBL
make figures    redraw the README charts
make clean      remove build output
```
