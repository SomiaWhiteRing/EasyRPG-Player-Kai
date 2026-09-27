package org.easyrpg.player.imports;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;
import androidx.recyclerview.widget.GridLayoutManager;

import org.easyrpg.player.R;
import org.easyrpg.player.imports.WebImportClient.Stage;
import org.easyrpg.player.imports.WebImportDownloads.Download;

import java.util.ArrayList;
import java.util.List;

/** Pending cards share the game list but expose no launch, settings or favorite actions. */
public final class WebImportAdapter extends RecyclerView.Adapter<WebImportAdapter.Holder> {
    private final List<Download> downloads = new ArrayList<>();

    public void submit(List<Download> snapshot) {
        List<Download> pending = new ArrayList<>();
        for (Download download : snapshot) if (download.stage != Stage.COMPLETE) pending.add(download);
        boolean sameIds = pending.size() == downloads.size();
        for (int i = 0; sameIds && i < pending.size(); i++) sameIds = pending.get(i).id.equals(downloads.get(i).id);
        downloads.clear();
        downloads.addAll(pending);
        if (sameIds) notifyItemRangeChanged(0, downloads.size(), "progress");
        else notifyDataSetChanged();
    }

    @Override public int getItemCount() { return downloads.size(); }

    @NonNull @Override public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.browser_download_card, parent, false);
        RecyclerView.LayoutManager manager = ((RecyclerView) parent).getLayoutManager();
        if (manager instanceof GridLayoutManager && ((GridLayoutManager) manager).getSpanCount() > 1) {
            // Match browser_game_card_landscape in the multi-column game grid.
            float density = parent.getResources().getDisplayMetrics().density;
            ViewGroup.LayoutParams params = view.getLayoutParams();
            params.width = Math.round(280 * density);
            params.height = Math.round(210 * density);
            view.setLayoutParams(params);
        }
        return new Holder(view);
    }

    @Override public void onBindViewHolder(@NonNull Holder holder, int position) {
        Download download = downloads.get(position);
        android.content.Context context = holder.itemView.getContext();
        holder.title.setText(download.metadata.title);
        holder.title.setEnabled(false);
        holder.cover.setEnabled(false);
        holder.cover.setAlpha(0.5f);
        holder.cover.setImageBitmap(download.cover);
        if (download.cover == null) holder.cover.setImageResource(R.drawable.ic_gamepad_black);
        String status = context.getString(download.message);
        String info = download.stage == Stage.DOWNLOADING ? download.transferText(context)
                : download.resumable() ? status + " \u00b7 " + download.transferText(context) : status;
        holder.status.setText(info);
        holder.status.setContentDescription(info);
        androidx.appcompat.widget.TooltipCompat.setTooltipText(holder.status, info);
        holder.progress.setIndeterminate(download.indeterminate());
        holder.progress.setProgress(download.percent);
        holder.action.setEnabled(download.stage != Stage.PAUSING && download.stage != Stage.REMOVING);
        holder.action.setContentDescription(context.getString(download.stage == Stage.ERROR ? R.string.web_import_retry :
                download.stage == Stage.PAUSED ? R.string.web_import_resume : R.string.web_import_pause));
        holder.action.setImageResource(download.stage == Stage.ERROR ? R.drawable.ic_download_retry :
                download.stage == Stage.PAUSED ? R.drawable.ic_download_resume : R.drawable.ic_download_pause);
        androidx.appcompat.widget.TooltipCompat.setTooltipText(holder.action, holder.action.getContentDescription());
        holder.remove.setVisibility(download.resumable() ? View.VISIBLE : View.GONE);
        holder.remove.setOnClickListener(v -> WebImportDownloads.get(context).remove(download.id));
        holder.action.setOnClickListener(v -> {
            WebImportDownloads queue = WebImportDownloads.get(context);
            if (download.resumable()) {
                queue.resume(download.id);
                WebImportService.start(context);
            } else queue.pause(download.id);
        });
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final TextView title, status;
        final ImageView cover;
        final ProgressBar progress;
        final ImageButton action, remove;
        Holder(View view) {
            super(view);
            title = view.findViewById(R.id.download_title);
            status = view.findViewById(R.id.download_status);
            cover = view.findViewById(R.id.download_cover);
            progress = view.findViewById(R.id.download_progress);
            action = view.findViewById(R.id.download_action);
            remove = view.findViewById(R.id.download_remove);
        }
    }
}
