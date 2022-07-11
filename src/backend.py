import os
from tracemalloc import start
from json import load, dump, dumps
import webview
import paws
from time import time

from functools import wraps

from flask import Flask, render_template, jsonify, request

server = Flask(
    __name__,
    static_folder=os.path.dirname(__file__),
    template_folder=os.path.dirname(__file__),
)
server.config["SEND_FILE_MAX_AGE_DEFAULT"] = 1  # disable caching

winObj = None


def setWinObj(obj):
    global winObj

    winObj = obj


@server.route("/")
def landing():
    return render_template("index.html", token=webview.token)


CONFIG_FILE = os.path.expanduser("~/.paws-config")


@server.route("/api/getConfig")
def getConfig():
    try:
        # Create config file if not exist
        if not os.path.exists(CONFIG_FILE):
            with open(CONFIG_FILE, "w") as f:
                dump([], f)

        # Get requested button id
        btnIdx = request.args.get("btnIdx")

        if btnIdx is not None:
            btnIdx = int(btnIdx)

        with open(CONFIG_FILE, "r") as f:
            config = load(f)

            if btnIdx is None:
                return {"success": True, "config": config}
            elif btnIdx >= len(config):
                return {"success": True, "config": {}}
            else:
                return {"success": True, "config": config[btnIdx]}
    except Exception as e:
        print("Error", str(e))
        return {"success": False, "Error": str(e)}


@server.route("/api/setConfig", methods=["POST"])
def setConfig():
    currConfig = request.get_json()

    # Read the config
    with open(CONFIG_FILE, "r") as f:
        config = load(f)

    btnIdx = int(currConfig["btnIdx"])

    print("pressedColor:", currConfig["pressedColor"])

    # Modify the config
    config[btnIdx]["pressedColor"] = currConfig["pressedColor"]
    config[btnIdx]["animation"] = currConfig["animation"]
    config[btnIdx]["animationColor"] = currConfig["animationColor"]
    config[btnIdx]["bindings"] = currConfig["bindings"]

    with open(CONFIG_FILE, "w") as f:
        dump(config, f)

    # Serialize the config and dump to device
    c = paws.json2conf(config)

    s = paws.probePort()

    paws.writeConfig(s, c)

    s.close()

    return {"success": True}


@server.route("/api/btnNum")
def btnNum():
    try:
        s = paws.probePort()

        num = paws.readNumberOfModules(s)

        s.close()

        if winObj is not None:
            if num <= 4:
                btnsInRow = 2
            elif num < 12:
                btnsInRow = 3
            else:
                btnsInRow = 4

            startOffsetX = 15
            startOffsetY = 40
            btnSize = 100
            padSize = 10

            winObj.resize(
                600,
                startOffsetY * 2 + (num // btnsInRow) * (padSize * 2 + btnSize) + 50,
            )

            # winObj.resize(
            #    startOffsetX * 2 + btnsInRow * (padSize * 2 + btnSize),
            #    startOffsetY * 2 + (num // btnsInRow) * (padSize * 2 + btnSize),
            # )

        return {"success": True, "btnNum": num}
    except:
        return {"success": False}


@server.route("/api/toggleBtnPrompt")
def toggleBtnPrompt():
    btnIdx = -1

    try:
        s = paws.probePort()

        paws.toggleBtnPrompt(s)

        print("Reading keys...")

        startTime = int(time())

        while True:
            try:
                btnIdx = int(s.read(1)[0])

                print("Pressed: %d" % (int(btnIdx)))

                break
            except:
                continue

        s.close()

        return {"success": True, "pressedIdx": btnIdx}
    except Exception as e:
        return {"success": False, "message": str(e)}


if __name__ == "__main__":
    server.run()
