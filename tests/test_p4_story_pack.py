"""KPB1 offline SD book compiler + compiled C++ reader integration.

All WAV used in these tests is synthetic signal and is NOT publishable child
narration. Production packages require adult-reviewed spoken WAV recordings.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib

from PIL import Image, ImageDraw

from tools.story_pack import FRAME_BYTES, HEADER, RECORD, make_pack, render_page


ROOT = Path(__file__).resolve().parents[1]


class StoryPackTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.base=Path(self.temp.name)
        self.src=self.base/"book.json"
        self.output=self.base/"forest_friends.kpb"
        self.pages=[]
        for i, text in enumerate(("小兔子遇见了小鸟。", "小鸟说你好。"), 1):
            picture=Image.new("RGB",(800,480),"white")
            draw=ImageDraw.Draw(picture)
            draw.rectangle((0,0,7,0),fill="black")
            draw.rectangle((110+i*10,140,250,260),outline="black",width=4)
            picture.save(self.base/f"{i}.png")
            with wave.open(str(self.base/f"{i}.wav"),"wb") as audio:
                audio.setnchannels(1)
                audio.setsampwidth(2)
                audio.setframerate(16000)
                # Fixture tone/sample, not a narration to deliver to children.
                audio.writeframes(b"\x00\x00"*4000)
            self.pages.append({
                "number":i, "text":text, "approved":True,
                "image":f"{i}.png", "audio":f"{i}.wav"
            })
        self.doc={
            "schema_version":1,"book_id":"forest_friends","title":"森林朋友",
            "approved":True,"pages":self.pages
        }
        self._save()

    def tearDown(self):
        self.temp.cleanup()

    def _save(self):
        self.src.write_text(json.dumps(self.doc,ensure_ascii=False),encoding="utf-8")

    def test_python_header_file_layout_repeatability(self):
        with self.assertRaisesRegex(ValueError,"--reviewed"):
            make_pack(self.src,self.output,reviewed=False)
        self.assertFalse(self.output.exists())
        result=make_pack(self.src,self.output,reviewed=True)
        blob=self.output.read_bytes()
        self.assertEqual(len(blob),HEADER.size+2*RECORD.size+len(blob)-160)
        header=HEADER.unpack_from(blob)
        self.assertEqual(header[:5],(b"KPB1",1,800,480,2))
        self.assertEqual(header[5:9],(32,128,160,len(blob)))
        self.assertEqual(header[9],zlib.crc32(blob[:28]))
        self.assertEqual(hashlib.sha256(blob).hexdigest(),result["sha256"])
        self.assertEqual(RECORD.size,64)
        self.assertEqual(RECORD.unpack_from(blob,32)[3],FRAME_BYTES)
        self.assertEqual(result["pages"][0]["duration_ms"],250)
        self.assertEqual(make_pack(self.src,self.output,reviewed=True)["sha256"],
                         result["sha256"])
        self.assertEqual(json.loads(self.output.with_suffix(".manifest.json").
                                   read_text(encoding="utf-8"))["approved"],True)

    def test_compiled_cpp_reader_cross_language_and_corruption(self):
        make_pack(self.src,self.output,reviewed=True)
        binary=self.base/"reader_check"
        command=[
            "g++","-std=c++17","-O2","-Wall","-Wextra","-Werror",
            "-I"+str(ROOT/"firmware/components/kid_app/include"),
            str(ROOT/"firmware/components/kid_app/src/story_pack_reader.cc"),
            str(ROOT/"firmware/components/kid_app/src/story_pack_file_port.cc"),
            str(ROOT/"tests/cpp/test_story_pack_reader.cc"),
            "-o",str(binary)
        ]
        subprocess.run(command,cwd=ROOT,check=True,capture_output=True,text=True,timeout=50)
        checked=subprocess.run([str(binary),str(self.base),"forest_friends"],
                               check=True,capture_output=True,text=True,timeout=12)
        self.assertIn("PASS: SD KPB1",checked.stdout)

    def test_unapproved_or_malformed_source_rejected(self):
        self.doc["approved"]=False
        self._save()
        with self.assertRaisesRegex(ValueError,"approved"):
            make_pack(self.src,self.output,reviewed=True)
        self.doc["approved"]=True
        self.doc["pages"][0]["approved"]=False
        self._save()
        with self.assertRaisesRegex(ValueError,"review"):
            make_pack(self.src,self.output,reviewed=True)
        self.doc["pages"][0]["approved"]=True
        self.doc["pages"][0]["image"]="../outside.png"
        self._save()
        with self.assertRaisesRegex(ValueError,"relative"):
            make_pack(self.src,self.output,reviewed=True)
        self.doc["pages"][0]["image"]="1.png"
        self.doc["pages"][0]["text"]="x"*321
        self._save()
        with self.assertRaisesRegex(ValueError,"Reviewed text"):
            make_pack(self.src,self.output,reviewed=True)
        self.assertFalse(self.output.exists())

    def test_wrong_audio_and_image_rejected(self):
        with wave.open(str(self.base/"1.wav"),"wb") as audio:
            audio.setnchannels(2)
            audio.setsampwidth(2)
            audio.setframerate(16000)
            audio.writeframes(b"\0\0\0\0"*4000)
        with self.assertRaisesRegex(ValueError,"mono"):
            make_pack(self.src,self.output,reviewed=True)
        with wave.open(str(self.base/"1.wav"),"wb") as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(16000)
            audio.writeframes(b"\0\0"*4000)
        Image.new("RGB",(600,480),"white").save(self.base/"1.png")
        with self.assertRaisesRegex(ValueError,"800 x 480"):
            make_pack(self.src,self.output,reviewed=True)

    def test_no_directory_symlink_escape(self):
        outside=self.base.parent/"outside-unreviewed.png"
        # Test stays fully inside this temporary workspace; symlink points
        # to a sibling of the book directory which must still be forbidden.
        with tempfile.TemporaryDirectory() as sibling:
            outside=Path(sibling)/"secret.png"
            Image.new("RGB",(800,480),"white").save(outside)
            link=self.base/"outside.png"
            try:
                link.symlink_to(outside)
            except (NotImplementedError,OSError):
                self.skipTest("Symbolic links are unavailable")
            self.doc["pages"][0]["image"]="outside.png"
            self._save()
            with self.assertRaisesRegex(ValueError,"outside"):
                make_pack(self.src,self.output,reviewed=True)

    def test_render_uses_explicit_font(self):
        # No font file is shipped; a developer supplies a properly licensed
        # CJK TTF/OTF for production. Missing paths must fail clearly.
        with self.assertRaisesRegex(ValueError,"licensed CJK"):
            render_page(self.base/"render.png",self.base/"missing.ttf",
                        "绘本","小鸟说你好。",1)


if __name__=="__main__":
    unittest.main()
