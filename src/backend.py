import os
from tracemalloc import start
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
