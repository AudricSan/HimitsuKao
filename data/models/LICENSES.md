# Model licences

## face_detection_yunet_2023mar.onnx

- Model: YuNet face detector (2023 March version, float32 ONNX)
- Source: https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet
  (file: https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx)
- Authors: Wei Wu, Hanyang Peng, Shiqi Yu (ShiqiYu/libfacedetection.train)
- Licence: MIT

The MIT licence text is available at
https://github.com/opencv/opencv_zoo/blob/main/models/face_detection_yunet/LICENSE
and applies to this file.

## OpenCV (linked statically)

The plugin statically links OpenCV 4.10.0 (Apache License 2.0), https://opencv.org/license/.

## face_recognition_sface_2021dec.onnx

- Model: SFace face recognition (2021 December version, float32 ONNX), 128-dimension embeddings
- Source: https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface
  (file: https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx)
- Authors: Zhong Yaoyao, Deng Weihong (zhongyy/SFace)
- Licence: Apache License 2.0
  (https://github.com/opencv/opencv_zoo/blob/main/models/face_recognition_sface/LICENSE)

## Other third-party code

- nlohmann/json 3.11.3 (MIT), fetched at configure time.
- libjpeg-turbo (BSD-style / IJG / zlib), bundled with OpenCV, used to encode face thumbnails.
