#include "nanodet_mnn.hpp"

using namespace std;

NanoDet::NanoDet(const std::string &mnn_path,
                     int input_width, int input_length, int num_thread_,
                     float score_threshold_, float nms_threshold_)
{
    num_thread = num_thread_;
    in_w = input_width;
    in_h = input_length;
    score_threshold = score_threshold_;
    nms_threshold = nms_threshold_;

    NanoDet_interpreter = std::shared_ptr<MNN::Interpreter>(MNN::Interpreter::createFromFile(mnn_path.c_str()));
    MNN::ScheduleConfig config;
    config.numThread = num_thread;
    MNN::BackendConfig backendConfig;
    backendConfig.precision = (MNN::BackendConfig::PrecisionMode) 2;
    config.backendConfig = &backendConfig;

    NanoDet_session = NanoDet_interpreter->createSession(config);

    input_tensor = NanoDet_interpreter->getSessionInput(NanoDet_session, nullptr);

}

NanoDet::~NanoDet()
{
    NanoDet_interpreter->releaseModel();
    NanoDet_interpreter->releaseSession(NanoDet_session);
}

int NanoDet::detect(cv::Mat &raw_image, std::vector<BoxInfo> &result_list)
{
    if (raw_image.empty()) {
        std::cout << "image is empty ,please check!" << std::endl;
        return -1;
    }
    image_h = raw_image.rows;
    image_w = raw_image.cols;
    cv::Mat image;
    cv::resize(raw_image, image, cv::Size(in_w, in_h));
    NanoDet_interpreter->resizeTensor(input_tensor, {1, 3, in_h, in_w});
    NanoDet_interpreter->resizeSession(NanoDet_session);
    std::shared_ptr<MNN::CV::ImageProcess> pretreat(
        MNN::CV::ImageProcess::create(MNN::CV::BGR, MNN::CV::BGR, mean_vals, 3,
                                        norm_vals, 3));
    pretreat->convert(image.data, in_w, in_h, image.step[0], input_tensor);

    auto start = chrono::steady_clock::now();


    // run network
    NanoDet_interpreter->runSession(NanoDet_session);

    // get output data
    std::vector<std::vector<BoxInfo>> results;
    results.resize(num_class);

    for (const auto &head_info : heads_info)
    {
        MNN::Tensor *tensor_scores = NanoDet_interpreter->getSessionOutput(NanoDet_session, head_info.cls_layer.c_str());
        MNN::Tensor *tensor_boxes = NanoDet_interpreter->getSessionOutput(NanoDet_session, head_info.dis_layer.c_str());

        MNN::Tensor tensor_scores_host(tensor_scores, tensor_scores->getDimensionType());
        tensor_scores->copyToHostTensor(&tensor_scores_host);

        MNN::Tensor tensor_boxes_host(tensor_boxes, tensor_boxes->getDimensionType());
        tensor_boxes->copyToHostTensor(&tensor_boxes_host);

        decode_infer(&tensor_scores_host, &tensor_boxes_host, head_info.stride, score_threshold, results);
    }

    auto end = chrono::steady_clock::now();
    chrono::duration<double> elapsed = end - start;
    cout << "inference time:" << elapsed.count() << " s, ";

    // std::vector<BoxInfo> dets;
    for (int i = 0; i < (int)results.size(); i++)
    {
        nms(results[i], nms_threshold);

        for (auto box : results[i])
        {
            box.x1 = box.x1 / in_w * image_w;
            box.x2 = box.x2 / in_w * image_w;
            box.y1 = box.y1 / in_h * image_h;
            box.y2 = box.y2 / in_h * image_h;
            result_list.push_back(box);
        }
    }
    cout << "detect " << result_list.size() << " objects" << endl;

    return 0;
}

void NanoDet::decode_infer(MNN::Tensor *cls_pred, MNN::Tensor *dis_pred, int stride, float threshold, std::vector<std::vector<BoxInfo>> &results)
{
    int feature_h = in_h / stride;
    int feature_w = in_w / stride;

    //cv::Mat debug_heatmap = cv::Mat(feature_h, feature_w, CV_8UC3);
    for (int idx = 0; idx < feature_h * feature_w; idx++)
    {
        // scores is a tensor with shape [feature_h * feature_w, num_class]
        const float *scores = cls_pred->host<float>() + (idx * num_class);

        int row = idx / feature_w;
        int col = idx % feature_w;
        float score = 0;
        int cur_label = 0;
        for (int label = 0; label < num_class; label++)
        {
            if (scores[label] > score)
            {
                score = scores[label];
                cur_label = label;
            }
        }
        if (score > threshold)
        {
            //std::cout << "label:" << cur_label << " score:" << score << std::endl;
            // bbox is a tensor with shape [feature_h * feature_w, 4_points * 8_distribution_bite]
            const float *bbox_pred = dis_pred->host<float>() + (idx * 4 * (reg_max + 1));
            results[cur_label].push_back(disPred2Bbox(bbox_pred, cur_label, score, col, row, stride));
            //debug_heatmap.at<cv::Vec3b>(row, col)[0] = 255;
            //cv::imshow("debug", debug_heatmap);
        }
    }
}

BoxInfo NanoDet::disPred2Bbox(const float *&dfl_det, int label, float score, int x, int y, int stride)
{
    float ct_x = (x + 0.5) * stride;
    float ct_y = (y + 0.5) * stride;
    std::vector<float> dis_pred;
    dis_pred.resize(4);
    for (int i = 0; i < 4; i++)
    {
        float dis = 0;
        float *dis_after_sm = new float[reg_max + 1];
        activation_function_softmax(dfl_det + i * (reg_max + 1), dis_after_sm, reg_max + 1);
        for (int j = 0; j < reg_max + 1; j++)
        {
            dis += j * dis_after_sm[j];
        }
        dis *= stride;
        //std::cout << "dis:" << dis << std::endl;
        dis_pred[i] = dis;
        delete[] dis_after_sm;
    }
    float xmin = (std::max)(ct_x - dis_pred[0], .0f);
    float ymin = (std::max)(ct_y - dis_pred[1], .0f);
    float xmax = (std::min)(ct_x + dis_pred[2], (float)in_w);
    float ymax = (std::min)(ct_y + dis_pred[3], (float)in_h);

    //std::cout << xmin << "," << ymin << "," << xmax << "," << xmax << "," << std::endl;
    return BoxInfo{xmin, ymin, xmax, ymax, score, label};
}

void NanoDet::nms(std::vector<BoxInfo> &input_boxes, float NMS_THRESH)
{
    std::sort(input_boxes.begin(), input_boxes.end(), [](BoxInfo a, BoxInfo b) { return a.score > b.score; });
    std::vector<float> vArea(input_boxes.size());
    for (int i = 0; i < int(input_boxes.size()); ++i)
    {
        vArea[i] = (input_boxes.at(i).x2 - input_boxes.at(i).x1 + 1) * (input_boxes.at(i).y2 - input_boxes.at(i).y1 + 1);
    }
    for (int i = 0; i < int(input_boxes.size()); ++i)
    {
        for (int j = i + 1; j < int(input_boxes.size());)
        {
            float xx1 = (std::max)(input_boxes[i].x1, input_boxes[j].x1);
            float yy1 = (std::max)(input_boxes[i].y1, input_boxes[j].y1);
            float xx2 = (std::min)(input_boxes[i].x2, input_boxes[j].x2);
            float yy2 = (std::min)(input_boxes[i].y2, input_boxes[j].y2);
            float w = (std::max)(float(0), xx2 - xx1 + 1);
            float h = (std::max)(float(0), yy2 - yy1 + 1);
            float inter = w * h;
            float ovr = inter / (vArea[i] + vArea[j] - inter);
            if (ovr >= NMS_THRESH)
            {
                input_boxes.erase(input_boxes.begin() + j);
                vArea.erase(vArea.begin() + j);
            }
            else
            {
                j++;
            }
        }
    }
}

string NanoDet::get_label_str(int label)
{
    return labels[label];
}

inline float fast_exp(float x)
{
    union
    {
        uint32_t i;
        float f;
    } v{};
    v.i = (1 << 23) * (1.4426950409 * x + 126.93490512f);
    return v.f;
}

inline float sigmoid(float x)
{
    return 1.0f / (1.0f + fast_exp(-x));
}

template <typename _Tp>
int activation_function_softmax(const _Tp *src, _Tp *dst, int length)
{
    const _Tp alpha = *std::max_element(src, src + length);
    _Tp denominator{0};

    for (int i = 0; i < length; ++i)
    {
        dst[i] = fast_exp(src[i] - alpha);
        denominator += dst[i];
    }

    for (int i = 0; i < length; ++i)
    {
        dst[i] /= denominator;
    }

    return 0;
}
