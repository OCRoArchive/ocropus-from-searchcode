#include "ocrorast.h"

void CTextlineRASTBasic::setDefaultParameters() {
    generation = 0;
    lsq        = 1;
    epsilon    = 5.0;
    maxsplits  = 1;
    delta      = 1.0;
    adelta     = 0.001;

    min_length = 100;
    min_gap    = 200;
    min_q      = 3.0;
    min_count  = 6;
    max_results = 1000;
    use_whitespace = true;

    splitscale[0] = 1.0;
    splitscale[1] = 2000.0;
    splitscale[2] = 1.0;
    all_params[0] = interval(0,6000);
    all_params[1] = interval(-0.05,0.05);
    all_params[2] = interval(0,20);
    empty_parameters[0] = interval(1,0);
    empty_parameters[1] = interval(1,0);
    empty_parameters[2] = interval(1,0);

}
CTextlineRASTBasic::CTextlineRASTBasic(){
    setDefaultParameters();
}

CTextlineRASTBasic::TLStateBasic::TLStateBasic() {
    depth = -(1<<14);
    quality = 0.0;
    matches.clear();
    splits = 0;
    splittable = true;
}

void CTextlineRASTBasic::setMaxSlope(double max_slope){
    all_params[1] = interval(-max_slope,max_slope);
}

void CTextlineRASTBasic::setMaxYintercept(double ymin, double ymax){
    all_params[0] = interval(ymin,ymax);
}

void CTextlineRASTBasic::TLStateBasic::set(CTextlineRASTBasic &line,
        int depth,Parameters &params,
        Matches &candidates,int splits) {
    this->splits = splits;
    this->splittable = (splits<line.maxsplits);
    this->depth = depth;
    this->params = params;
    rank = -1;
    matches.clear();
    update(line,candidates);
}

void CTextlineRASTBasic::TLStateBasic::reeval(CTextlineRASTBasic &line) {
    Matches temp,rest;
    copy(temp,matches);
    copy(matches,rest);
    //temp.swapwith(matches);
    update(line,temp);
}

void CTextlineRASTBasic::TLStateBasic::update(CTextlineRASTBasic &line,
        Matches &candidates) {
    matches.clear();
    quality = 0.0;
    interval r = params[0];
    interval m = params[1];
    interval d = params[2];

    bool descender_present=false;

    for(int i = 0;i<candidates.length();i++) {
        int bi = candidates[i];
        if(line.used[bi]) continue;

        rectangle &b = line.cboxes[bi];
        interval y = r + m * b.xcenter();
        interval q;
        if(b.y1<y.lo) {
            // if the top is below the baseline, it doesn't count
            q = 0;
        } else {
            // q based on the distance of the bottom from the baseline
            interval q1 = line.influence(line.lsq,abs(b.y0-y),line.epsilon);
            // q based on the distance of the bottom from the
            // descender line
            interval q2 = 0.75*line.influence(line.lsq,abs(b.y0-(y-d)),line.epsilon);
            q = max(q1,q2);
            if(q2.hi>0.0)
                descender_present = true;
        }

        // if it didn't match, see whether it's contained somewhere within the line
        if(false && q.hi == 0.0) {
            interval yc = r + m * b.ycenter();
            if(!(yc<y-d || yc>y+d)) q = interval(1e-3,1e-3);
        }

        if(q.hi>0.0) {
            matches.push(bi);
        }
        quality = quality + q;
    }

    // check for total length; we can do that because the cboxes
    // are in sorted order
    if(matches.length() >= 2) {
        int start = matches[0];
        int end = matches[matches.length()-1];
        float length = line.cboxes[end].x1-line.cboxes[start].x0;
        if(length<line.min_length) quality = 0;
    } else {
        quality = 0;
    }

    // If no descenders are present, set the distance to zero
    if(!descender_present)
        params[2] = 0.0;
    priority = quality.hi;
    generation = line.generation;
}

TextLineParam CTextlineRASTBasic::TLStateBasic::returnLineParam(){
    TextLineParam tl;
    tl.c = params[0].center();
    tl.m = params[1].center();
    tl.d = params[2].center();
    return tl;
}

void CTextlineRASTBasic::prepare() {
    all_matches.clear();
    for(int i = 0;i<cboxes.length();i++)
        all_matches.push(i);

    used.resize(cboxes.length());
    fill(used,false);

    CState initial;
    initial->set(*this,0,all_params,all_matches,0);

    queue.clear();
    queue.insert(initial,initial->priority);
}

void CTextlineRASTBasic::makeSubStates(narray<CState> &substates,CState &state) {
    substates.clear();
    Parameters &p = state->params;

    int mi = -1;
    double mv = -1e30;
    for(int i = 0;i<ntlparams;i++) {
        double v = p[i].width()*splitscale[i];
        if(v>mv) { mi = i; mv = v; }
    }

    Parameters left = state->params.split(mi,0);
    CState sleft;
    sleft->set(*this,state->depth+1,left,state->matches,state->splits);
    substates.push(sleft);

    Parameters right = state->params.split(mi,1);
    CState sright;
    sright->set(*this,state->depth+1,right,state->matches,state->splits);
    substates.push(sright);
}

//Assuming horizontal lines with slope in the interval [-0.05, 0.05]
int CTextlineRASTBasic::wboxIntersection(CState &top){

    Matches &matches = top->matches;

    if(matches.length() <= 1) return -1;

    float start = (int) cboxes[matches[0]].xcenter();
    float end = (int) cboxes[matches[matches.length()-1]].xcenter();

    for(int i = 0; i < wboxes.length(); i++){
        interval y = top->params[1]*wboxes[i].x0 + top->params[0];
        //If the box cuts the whole parameter interval
        if( (y.lo >= wboxes[i].y0) && (y.hi <= wboxes[i].y1) )
            if( (start < wboxes[i].x0) && (end > wboxes[i].x1) )
                return i;
    }
    return -1;
}

void CTextlineRASTBasic::search() {
    for(int iter = 0;;iter++) {
        if(results.length() >= max_results) break;
        if(queue.length()<1) break;
        CState top;
        top = queue.extractMax();
        if(top->generation != generation) {
            top->reeval(*this);
            if(top->quality.hi<min_q) continue;
            if(top->matches.length()<min_count) continue;
            queue.insert(top,top->priority);
            continue;
        }
        if(use_whitespace){
            Matches &matches = top->matches;
            int index = wboxIntersection(top);
            if(index >= 0){
                Matches leftmatches,rightmatches;
                for(int i = 0;i<matches.length();i++) {
                    if(cboxes[matches[i]].xcenter() < wboxes[index].x0)
                        leftmatches.push(matches[i]);
                    if(cboxes[matches[i]].xcenter() > wboxes[index].x1)
                        rightmatches.push(matches[i]);
                }
                CState sleft,sright;
                sleft->set(*this,top->depth+1,top->params,leftmatches,top->splits+1);
                sright->set(*this,top->depth+1,top->params,rightmatches,top->splits+1);
                queue.insert(sleft,sleft->priority);
                queue.insert(sright,sright->priority);
                continue;
            }
        }
        float threshold = min_gap;
        if(threshold>0){
            Matches &matches = top->matches;
            int mi = -1; float mgap = -1e38;
            for(int i = 1;i<matches.length();i++) {
                float gap = cboxes[matches[i]].x0 - cboxes[matches[i-1]].x1;
                if(gap <= mgap) continue;
                mgap = gap;
                mi = i;
            }
            if(mgap>threshold) {
                Matches leftmatches,rightmatches;
                for(int i = 0;i<matches.length();i++) {
                    if(i<mi) leftmatches.push(matches[i]);
                    else rightmatches.push(matches[i]);
                }
                CState sleft,sright;
                sleft->set(*this,top->depth+1,top->params,leftmatches,top->splits+1);
                sright->set(*this,top->depth+1,top->params,rightmatches,top->splits+1);
                queue.insert(sleft,sleft->priority);
                queue.insert(sright,sright->priority);
                continue;
            }
        }
        if(final(top->quality,top->params)) {
            pushResult(top);
            continue;
        }
        narray<CState> substates;
        makeSubStates(substates,top);
        for(int i = 0;i<substates.length();i++) {
            if(substates[i]->quality.hi<min_q) continue;
            if(substates[i]->matches.length()<min_count) continue;
            queue.insert(substates[i],substates[i]->priority);
        }
    }
}

void CTextlineRASTBasic::pushResult(CState &result){
    Matches &matches = result->matches;
    for(int i = 0;i<matches.length();i++)
        used[matches[i]] = true;

    results.push(result);
    generation++;
}

void CTextlineRASTBasic::extract(narray<TextLineParam> &textlines,
        rectarray &columns,
        autodel<CharStats> &charstats){
    if(charstats->char_boxes.length() <= 1)
        return;
    linestats = make_CharStats(*charstats);
    for(int i = 0, l = charstats->char_boxes.length(); i<l; i++){
        // Only consider those char boxes that do not overlap with any
        // obstacle, i.e. not contained in any obstacle
        bool contained=false;
        for(int j=0; j<columns.length(); j++){
            if(charstats->char_boxes[i].fraction_covered_by(columns[j]) > 0.5){
                contained=true;
                break;
            }
        }
        if(!contained)
            cboxes.push(charstats->char_boxes[i]);
    }
    for(int i = 0, l = columns.length(); i<l; i++){
        wboxes.push(columns[i]);
    }

    sort_boxes_by_x0(cboxes);
    prepare();
    search();

    for(int i = 0, l = results.length(); i<l; i++) {
        textlines.push(results[i]->returnLineParam());
    }
}

void CTextlineRASTBasic::extract(narray<TextLineParam> &textlines,
        autodel<CharStats> &charstats){
    if(charstats->char_boxes.length() <= 1)
        return;
    linestats = make_CharStats(*charstats);
    for(int i = 0, l = charstats->char_boxes.length(); i<l; i++){
        cboxes.push(charstats->char_boxes[i]);
    }
    use_whitespace = false;

    sort_boxes_by_x0(cboxes);
    prepare();
    search();

    for(int i = 0, l = results.length(); i<l; i++) {
        textlines.push(results[i]->returnLineParam());
    }
}
CTextlineRASTBasic *make_CTextlineRASTBasic(){
    return new CTextlineRASTBasic();
}

void CTextlineRAST::setDefaultParameters() {
    generation = 0;
    all_params[0] = interval(0,6000);
    all_params[1] = interval(-0.05,0.05);
    all_params[2] = interval(0,20);
    empty_parameters[0] = interval(1,0);
    empty_parameters[1] = interval(1,0);
    empty_parameters[2] = interval(1,0);

    lsq = 1;
    epsilon = 5.0;

    min_length = 100;
    min_gap = 200;
    maxsplits = 1;

    min_q = 3.0;
    min_count = 6;

    delta = 1.0;
    adelta = 0.001;
    minoverlap = 0.9;

    //rejection threshold for the height of a box = tr*xheight
    min_box_height = 3.1;
    extend = 0;
    max_results = 1000;
    word_gap = 30; //Maximum distance between words
    min_height = 5;
    assign_boxes = 1;

    aggressive = true;
    use_whitespace = true;
    splitscale[0] = 1.0;
    splitscale[1] = 2000.0;
    splitscale[2] = 1.0;

}
CTextlineRAST::CTextlineRAST(){
    setDefaultParameters();
}

void CTextlineRAST::pushResult(CState &result){
    Matches &matches = result->matches;
    for(int i = 0;i<matches.length();i++)
        used[matches[i]] = true;

    //Estimate xheight of the line
    rectarray line_elements;
    rectangle line_region = rectangle();
    for(int i = 0;i<matches.length();i++){
        line_elements.push(cboxes[matches[i]]);
        line_region.include(cboxes[matches[i]]);
    }
    int xheight = calc_xheight(line_elements);
    if(xheight <= min_height)
        return;

    //Initialize result line
    TextLineParam tlp = result->returnLineParam();
    TextLine tl = TextLine(tlp);
    tl.xheight  = xheight;

    //Calculate bounding box of the line
    tl.bbox = rectangle();
    for(int i = 0;i<matches.length();i++){
        float box_height = cboxes[matches[i]].y1 - cboxes[matches[i]].y0;
        if(box_height > min_box_height * xheight){
            used[matches[i]] = false;
            continue;
        }
        tl.bbox.include(cboxes[matches[i]]);
    }

    //Assign all char_boxes contained in the line bb to the line
    rectangle temp = rectangle(tl.bbox);
    if(assign_boxes){
        temp.x0 -= word_gap;
        temp.x1 += word_gap;
        temp.y0 -= 3;
        temp.y1 += 3;
        for(int i = 0;i<cboxes.length();i++){
            if( (!used[i]) && (cboxes[i].fraction_covered_by(temp) > minoverlap) ){
                tl.bbox.include(cboxes[i]);
                used[i] = true;
            }
        }
        if(aggressive){
            int all_len = cboxes_all.length();
            for(int i = 0;i<all_len;i++){
                if( !used_all[i] && cboxes_all[i].fraction_covered_by(temp)>minoverlap){
                    tl.bbox.include(cboxes_all[i]);
                    used_all[i] = true;
                }
            }
        }
    }
    if(extend){
        temp = rectangle(tl.bbox);
        int new_start = 0;
        int new_end = pagewidth-1;

        for(int j = 0; j<wboxes.length(); j++){
            if( (temp.y0 < wboxes[j].y1) && (temp.y1 > wboxes[j].y0) ) {
                //                if(wbox_intersection(lines[i], wboxes[j])){
                if(wboxes[j].x1 <= temp.x0)
                    new_start = (new_start > wboxes[j].x1) ? new_start : wboxes[j].x1;
                else if(wboxes[j].x0 >= temp.x1)
                    new_end = (new_end   < wboxes[j].x0) ? new_end   : wboxes[j].x0;
            }
            }
            new_start = max(new_start,temp.x0-min_gap);
            new_end = min(new_end,temp.x1+min_gap);
            temp.x0 = (temp.x0 > new_start) ? new_start : temp.x0;
            temp.x1 = (temp.x1 < new_end  ) ? new_end   : temp.x1;
            tl.bbox = rectangle(temp);
        }

        result_lines.push(tl);
        results.push(result);
        generation++;
    }

    void CTextlineRAST::extract(narray<TextLine>    &textlines,
            rectarray           &columns,
            autodel<CharStats>  &charstats){
        if(charstats->char_boxes.length() <= 1)
            return;
        linestats = make_CharStats(*charstats);
        int dist = int (charstats->word_spacing*1.5);
        word_gap = (word_gap < dist) ? word_gap : dist;
        pagewidth = charstats->img_width;
        pageheight = charstats->img_height;
        result_lines.clear();
        cboxes.clear();
        cboxes_all.clear();
        for(int i = 0, l = charstats->char_boxes.length(); i<l; i++){
            // Only consider those char boxes that do not overlap with any
            // obstacle, i.e. not contained in any obstacle
            bool contained=false;
            for(int j=0; j<columns.length(); j++){
                if(charstats->char_boxes[i].fraction_covered_by(columns[j]) > 0.5){
                    contained=true;
                    break;
                }
            }
            if(!contained)
                cboxes.push(charstats->char_boxes[i]);
        }
        used.resize(cboxes.length());
        fill(used,false);
        for(int i = 0, l = charstats->dot_boxes.length(); i<l; i++)
            cboxes_all.push(charstats->dot_boxes[i]);
        used_all.resize(cboxes_all.length());
        fill(used_all,false);
        for(int i = 0, l = columns.length(); i<l; i++)
            wboxes.push(columns[i]);

        sort_boxes_by_x0(cboxes);
        prepare();
        search();
        copy(textlines,result_lines);
    }

    void CTextlineRAST::extract(narray<TextLine>      &textlines,
            autodel<CharStats>    &charstats){
        if(charstats->char_boxes.length() <= 1)
            return;
        linestats = make_CharStats(*charstats);
        int dist = int (charstats->word_spacing*1.5);
        word_gap = (word_gap < dist) ? word_gap : dist;
        pagewidth = charstats->img_width;
        pageheight = charstats->img_height;
        result_lines.clear();
        cboxes.clear();
        cboxes_all.clear();
        for(int i = 0, l = charstats->char_boxes.length(); i<l; i++)
            cboxes.push(charstats->char_boxes[i]);
        used.resize(cboxes.length());
        fill(used,false);
        for(int i = 0, l = charstats->dot_boxes.length(); i<l; i++)
            cboxes_all.push(charstats->dot_boxes[i]);
        used_all.resize(cboxes_all.length());
        fill(used_all,false);
        use_whitespace = false;

        sort_boxes_by_x0(cboxes);
        prepare();
        search();
        copy(textlines,result_lines);
    }

    CTextlineRAST *make_CTextlineRAST() {
        return new CTextlineRAST();
    }
