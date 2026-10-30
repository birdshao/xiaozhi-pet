"""Reviewable examples; all texts are original and intentionally short.

Never automatically publish to a real child's device: importing with
--approved requires explicit trusted-adult review of every page/card.
"""
BOOK = {
    "book_id": "forest_friends",
    "title": "森林里的小伙伴",
    "pages": (
        "早晨，小兔子来到森林里。树叶轻轻摇晃。",
        "小兔子遇见小鸟。小鸟说：你好呀！",
        "小兔子和小鸟一起看云朵。今天真美好。",
    ),
}
QUIZZES = (
    ("animal_owl", "谁晚上也常常醒着？", "猫头鹰", "小蝴蝶", 1,
     "答对啦，猫头鹰常常在夜间活动。", "再想想，谁的大眼睛适合在夜晚观察呢？"),
    ("animal_elephant", "谁有长长的鼻子？", "小松鼠", "大象", 2,
     "答对啦，大象有灵活的长鼻子。", "再想一想，谁能用鼻子卷起东西呢？"),
    ("animal_frog", "谁喜欢在池塘边跳一跳？", "青蛙", "海星", 1,
     "答对啦，青蛙会跳，也会游泳。", "再试试看，谁的后腿善于跳跃呢？"),
    ("animal_bird", "谁有羽毛和翅膀？", "小乌龟", "小鸟", 2,
     "答对啦，小鸟有羽毛和翅膀。", "可以再想一想，谁有轻轻的羽毛呢？"),
)
