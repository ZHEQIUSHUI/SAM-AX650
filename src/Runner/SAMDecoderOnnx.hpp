#pragma once
#include "thread"
#include "iostream"

#include "opencv2/opencv.hpp"
#include "onnxruntime_cxx_api.h"

#include "sample_log.h"

struct MatInfo
{
    cv::Mat mask;
    float iou_pred;
};

class SAMDecoder
{
protected:
    std::vector<float> samfeature;
    std::vector<int64_t> samfeature_shape;
    int img_width, img_height;
    float encoder_scale = -1;

    virtual std::vector<MatInfo> Decoder_Inference(cv::Point *clickinfo, cv::Rect *boxinfo) = 0;

public:
    virtual int Load(std::string model_file, int nthread = 4)
    {
        ALOGE("no impl");
        return -1;
    };
    virtual int Load(std::string model_file_pts, std::string model_file_sub, int nthread = 4)
    {
        ALOGE("no impl");
        return -1;
    };

    std::vector<MatInfo> Inference(cv::Point pt)
    {
        return Decoder_Inference(&pt, nullptr);
    }

    std::vector<MatInfo> Inference(cv::Rect box)
    {
        cv::Point center((box.tl().x + box.br().x) / 2,
                         (box.tl().y + box.br().y) / 2);
        return Decoder_Inference(&center, &box);
    }

    void LoadFeature(int width, int height, float *feature, int feature_size, std::vector<unsigned int> feature_shape, float scale)
    {
        samfeature.resize(feature_size);
        memcpy(samfeature.data(), feature, feature_size * 4);

        samfeature_shape.resize(feature_shape.size());
        for (size_t i = 0; i < feature_shape.size(); i++)
        {
            samfeature_shape[i] = feature_shape[i];
        }

        img_width = width;
        img_height = height;
        encoder_scale = scale;
    }
};

class SAMDecoderOnnx : public SAMDecoder
{
protected:
    std::string device{"cpu"};
    Ort::Env env;
    Ort::SessionOptions session_options;
    std::shared_ptr<Ort::Session> DecoderSession;
    Ort::MemoryInfo memory_info_handler = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    const char *DecoderInputNames[6]{"image_embeddings", "point_coords", "point_labels",
                                     "mask_input", "has_mask_input", "orig_im_size"},
        *DecoderOutputNames[3]{"masks", "iou_predictions", "low_res_masks"};

    std::vector<MatInfo> Decoder_Inference(cv::Point *clickinfo, cv::Rect *boxinfo) override
    {
        int numPoints = boxinfo ? 3 : 1;

        std::vector<float> inputPointsValues = {
            (float)clickinfo->x * encoder_scale,
            (float)clickinfo->y * encoder_scale};
        // printf("%2.2f %2.2f %2.2f\n", inputPointsValues[0], inputPointsValues[1], encoder_scale);
        if (boxinfo)
        {
            inputPointsValues.push_back((float)boxinfo->tl().x * encoder_scale);
            inputPointsValues.push_back((float)boxinfo->tl().y * encoder_scale);

            inputPointsValues.push_back((float)boxinfo->br().x * encoder_scale);
            inputPointsValues.push_back((float)boxinfo->br().y * encoder_scale);
            // printf("%2.2f %2.2f %2.2f %2.2f\n", inputPointsValues[2], inputPointsValues[3], inputPointsValues[4], inputPointsValues[5]);

            // printf("%2.2f %2.2f %2.2f %2.2f\n", inputPointsValues[2], inputPointsValues[3], inputPointsValues[4], inputPointsValues[5]);
        }

        float inputLabelsValues[] = {1.0f, 2.0f, 3.0f};

        const size_t maskInputSize = 256 * 256;
        float maskInputValues[maskInputSize],
            hasMaskValues[] = {0},
            orig_im_size_values[] = {(float)img_height, (float)img_width};
        memset(maskInputValues, 0, sizeof(maskInputValues));

        std::vector<int64_t> inputPointShape = {1, numPoints, 2},
                             pointLabelsShape = {1, numPoints},
                             maskInputShape = {1, 1, 256, 256},
                             hasMaskInputShape = {1},
                             origImSizeShape = {2};

        std::vector<Ort::Value> inputTensorsSam;
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, samfeature.data(), samfeature.size(),
            samfeature_shape.data(), samfeature_shape.size()));

        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, inputPointsValues.data(), 2 * numPoints, inputPointShape.data(), inputPointShape.size()));
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, inputLabelsValues, 1 * numPoints, pointLabelsShape.data(), pointLabelsShape.size()));

        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, maskInputValues, maskInputSize, maskInputShape.data(), maskInputShape.size()));
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, hasMaskValues, 1, hasMaskInputShape.data(), hasMaskInputShape.size()));
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, orig_im_size_values, 2, origImSizeShape.data(), origImSizeShape.size()));

        Ort::RunOptions runOptionsSam;

        // std::cout << "=> Decoder Inference Start ..." << std::endl;
        auto time_start = std::chrono::high_resolution_clock::now();
        auto DecoderOutputTensors = DecoderSession->Run(runOptionsSam, DecoderInputNames, inputTensorsSam.data(),
                                                        inputTensorsSam.size(), DecoderOutputNames, 3);
        auto time_end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = time_end - time_start;
        // std::cout << "Decoder Inference Finish ..." << std::endl;
        std::cout << "Decoder Inference Cost time : " << diff.count() << "s" << std::endl;

        auto masks = DecoderOutputTensors[0].GetTensorMutableData<float>();
        auto iou_predictions = DecoderOutputTensors[1].GetTensorMutableData<float>();
        auto low_res_masks = DecoderOutputTensors[2].GetTensorMutableData<float>();

        Ort::Value &masks_ = DecoderOutputTensors[0];
        Ort::Value &iou_predictions_ = DecoderOutputTensors[1];
        Ort::Value &low_res_masks_ = DecoderOutputTensors[2];

        auto mask_dims = masks_.GetTypeInfo().GetTensorTypeAndShapeInfo().GetShape();
        auto iou_pred_dims = iou_predictions_.GetTypeInfo().GetTensorTypeAndShapeInfo().GetShape();
        auto low_res_dims = low_res_masks_.GetTypeInfo().GetTensorTypeAndShapeInfo().GetShape();

        const unsigned int Resizemasks_batch = mask_dims.at(0);
        const unsigned int Resizemasks_nums = mask_dims.at(1);
        const unsigned int Resizemasks_width = mask_dims.at(2);
        const unsigned int Resizemasks_height = mask_dims.at(3);

        // std::cout << "Resizemasks_batch : " << Resizemasks_batch << " Resizemasks_nums : " << Resizemasks_nums
        //           << " Resizemasks_width : " << Resizemasks_width << " Resizemasks_height : " << Resizemasks_height << std::endl;

        // std::cout << "Gemmiou_predictions_dim_0 : " << iou_pred_dims.at(0) << " Generate mask num : " << iou_pred_dims.at(1) << std::endl;

        // std::cout << "Reshapelow_res_masks_dim_0 : " << low_res_dims.at(0) << " Reshapelow_res_masks_dim_1 : " << low_res_dims.at(1) << std::endl;
        // std::cout << "Reshapelow_res_masks_dim_2 : " << low_res_dims.at(2) << " Reshapelow_res_masks_dim_3 : " << low_res_dims.at(3) << std::endl;

        std::vector<MatInfo> masks_list;
        for (unsigned int index = 0; index < Resizemasks_nums; index++)
        {
            cv::Mat mask(img_height, img_width, CV_8UC1);
            for (unsigned int i = 0; i < mask.rows; i++)
            {
                for (unsigned int j = 0; j < mask.cols; j++)
                {
                    mask.at<uchar>(i, j) = masks[i * mask.cols + j + index * mask.rows * mask.cols] > 0 ? 255 : 0;
                }
            }
            MatInfo mat_info;
            mat_info.mask = mask;
            mat_info.iou_pred = *(iou_predictions++);
            masks_list.emplace_back(mat_info);
        }
        return masks_list;
    }

public:
    int Load(std::string model_file, int nthread = 4) override
    {
        // 初始化OnnxRuntime运行环境
        env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SegmentAnythingModel");
        session_options = Ort::SessionOptions();
        session_options.SetInterOpNumThreads(std::thread::hardware_concurrency());
        session_options.SetIntraOpNumThreads(std::thread::hardware_concurrency());
        // 设置图像优化级别
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        DecoderSession.reset(new Ort::Session(env, model_file.c_str(), session_options));
        if (DecoderSession->GetInputCount() != 6 || DecoderSession->GetOutputCount() != 3)
        {
            ALOGE("Model not loaded (invalid input/output count)");
            return -1;
        }
        return 0;
    }
};

class SAMDecoderOnnxV2 : public SAMDecoderOnnx
{
protected:
    std::shared_ptr<Ort::Session> DecoderEncPtsSession;

    const char
        *DecoderPtsEncInputNmae[2] = {"point_coords", "point_labels"},
        *DecoderPtsEncOutputNmae[1] = {"/Add_5_output_0"},
        *DecoderInputNames[2]{"image_embeddings", "/Add_5_output_0"},
        *DecoderOutputNames[2]{"iou_predictions", "low_res_masks"};

    std::vector<Ort::Value> EncPts(cv::Point *clickinfo, cv::Rect *boxinfo)
    {
        // int numPoints = boxinfo ? 3 : 1;

        std::vector<float> inputLabelsValues = {1.0f, 0.0f, 0.0f};

        std::vector<float> inputPointsValues = {
            (float)clickinfo->x * encoder_scale,
            (float)clickinfo->y * encoder_scale};
        // printf("%2.2f %2.2f %2.2f\n", inputPointsValues[0], inputPointsValues[1], encoder_scale);
        if (boxinfo)
        {
            inputPointsValues.push_back((float)boxinfo->tl().x * encoder_scale);
            inputPointsValues.push_back((float)boxinfo->tl().y * encoder_scale);

            inputPointsValues.push_back((float)boxinfo->br().x * encoder_scale);
            inputPointsValues.push_back((float)boxinfo->br().y * encoder_scale);
            inputLabelsValues = {1.0f, 2.0f, 3.0f};
        }

        std::vector<int64_t> inputPointShape = {1, 3, 2},
                             pointLabelsShape = {1, 3};

        std::vector<Ort::Value> inputTensorsSam;

        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, inputPointsValues.data(), 2 * 3, inputPointShape.data(), inputPointShape.size()));
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, inputLabelsValues.data(), 1 * 3, pointLabelsShape.data(), pointLabelsShape.size()));

        Ort::RunOptions runOptionsSam;

        auto DecoderEncPtsOutputTensors = DecoderEncPtsSession->Run(runOptionsSam, DecoderPtsEncInputNmae, inputTensorsSam.data(),
                                                                    inputTensorsSam.size(), DecoderPtsEncOutputNmae, 1);
        return DecoderEncPtsOutputTensors;
    }

    std::vector<MatInfo> PostPorcess(float *iou_predictions, float *masks)
    {
        std::vector<MatInfo> masks_list;
        for (unsigned int index = 0; index < 4; index++)
        {
            cv::Mat tmp(256, 256, CV_32FC1, masks + index * 256 * 256), mask;
            cv::resize(tmp, mask, cv::Size(1024, 1024));
            // float *mask_ptr = mask.ptr<float>();
            for (unsigned int i = 0; i < mask.rows; i++)
            {
                for (unsigned int j = 0; j < mask.cols; j++)
                {
                    // if (mask.at<float>(i, j) > 0)
                    // {
                    //     printf("%f\n", mask.at<float>(i, j));
                    // }

                    //
                    mask.at<float>(i, j) = mask.at<float>(i, j) > 0 ? 255 : 0;
                }
            }
            mask.convertTo(mask, CV_8UC1);

            MatInfo mat_info;
            mat_info.mask = mask(cv::Rect(0, 0, MIN(mask.cols, encoder_scale * img_width), MIN(mask.rows, encoder_scale * img_height))).clone();
            cv::resize(mat_info.mask, mat_info.mask, cv::Size(img_width, img_height));
            mat_info.iou_pred = *(iou_predictions++);
            masks_list.emplace_back(mat_info);
        }
        return masks_list;
    }

    std::vector<MatInfo> Decoder_Inference(cv::Point *clickinfo, cv::Rect *boxinfo) override
    {

        auto DecoderEncPtsOutputTensors = EncPts(clickinfo, boxinfo);

        // auto masks = DecoderOutputTensors[0].GetTensorMutableData<float>();

        // Ort::Value &masks_ = DecoderOutputTensors[0];
        Ort::RunOptions runOptionsSam;
        std::vector<Ort::Value> inputTensorsSam;
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, samfeature.data(), samfeature.size(),
            samfeature_shape.data(), samfeature_shape.size()));
        // inputTensorsSam.push_back(Ort::Value(DecoderEncPtsOutputTensors[0]));
        inputTensorsSam.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler,
            DecoderEncPtsOutputTensors[0].GetTensorMutableData<float>(),
            DecoderEncPtsOutputTensors[0].GetTensorTypeAndShapeInfo().GetElementCount(),
            DecoderEncPtsOutputTensors[0].GetTensorTypeAndShapeInfo().GetShape().data(),
            DecoderEncPtsOutputTensors[0].GetTensorTypeAndShapeInfo().GetShape().size()));

        auto DecoderOutputTensors = DecoderSession->Run(runOptionsSam, DecoderInputNames, inputTensorsSam.data(),
                                                        inputTensorsSam.size(), DecoderOutputNames, 2);

        Ort::Value &iou_pred = DecoderOutputTensors[0];
        Ort::Value &low_res_masks_ = DecoderOutputTensors[1];

        float *iou_predictions = iou_pred.GetTensorMutableData<float>();
        float *masks = low_res_masks_.GetTensorMutableData<float>();

        // std::vector<MatInfo> masks_list;
        // for (unsigned int index = 0; index < 4; index++)
        // {
        //     cv::Mat tmp(256, 256, CV_32FC1, masks + index * 256 * 256), mask;
        //     cv::resize(tmp, mask, cv::Size(1024, 1024));

        //     // float *mask_ptr = mask.ptr<float>();
        //     for (unsigned int i = 0; i < mask.rows; i++)
        //     {
        //         for (unsigned int j = 0; j < mask.cols; j++)
        //         {
        //             mask.at<float>(i, j) = mask.at<float>(i, j) > 0 ? 255 : 0;
        //         }
        //     }
        //     mask.convertTo(mask, CV_8UC1);

        //     MatInfo mat_info;
        //     mat_info.mask = mask(cv::Rect(0, 0, MIN(mask.cols, encoder_scale * img_width), MIN(mask.rows, encoder_scale * img_height))).clone();
        //     cv::resize(mat_info.mask, mat_info.mask, cv::Size(img_width, img_height));
        //     mat_info.iou_pred = *(iou_predictions++);
        //     masks_list.emplace_back(mat_info);
        // }
        return PostPorcess(iou_predictions, masks);
    }

public:
    int Load(std::string model_file_pts, std::string model_file_sub, int nthread = 4) override
    {
        // 初始化OnnxRuntime运行环境
        env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "SegmentAnythingModel");
        session_options = Ort::SessionOptions();
        session_options.SetInterOpNumThreads(std::thread::hardware_concurrency());
        session_options.SetIntraOpNumThreads(std::thread::hardware_concurrency());
        // 设置图像优化级别
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        DecoderSession.reset(new Ort::Session(env, model_file_sub.c_str(), session_options));
        if (DecoderSession->GetInputCount() != 2 || DecoderSession->GetOutputCount() != 2)
        {
            ALOGE("DecoderSession Model not loaded (invalid input/output count)");
            return -1;
        }

        DecoderEncPtsSession.reset(new Ort::Session(env, model_file_pts.c_str(), session_options));
        if (DecoderEncPtsSession->GetInputCount() != 2 || DecoderEncPtsSession->GetOutputCount() != 1)
        {
            ALOGE("DecoderEncPtsSession Model not loaded (invalid input/output count)");
            return -1;
        }
        return 0;
    }
};
