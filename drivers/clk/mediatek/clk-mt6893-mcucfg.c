// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6893 / MT6891 MCUCFG CPU/DSU clock driver.
 *
 * Exposes the glitch-free CPU/DSU clock muxes in mcusys@0c53a000 and a
 * four-way ganged ARMPLL_BL clock that retunes armpll_bl0..3 in apmixedsys
 * without re-entering CCF prepare_lock.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/units.h>

#include <dt-bindings/clock/mediatek,mt6893-clk.h>

#include "clk-mtk.h"

#define CPU_PLLDIV_CFG0		0x2a0
#define CPU_PLLDIV_CFG1		0x2a4
#define CPU_PLLDIV_CFG2		0x2a8
#define CPU_PLLDIV_CFG3		0x2ac
#define CPU_PLLDIV_CFG4		0x2b0
#define BUS_PLLDIV_CFG		0x2e0

#define CPU_PLLDIV_MUX_MASK	GENMASK(10, 9)

#define ARMPLL_BL0_CON0		0x218
#define ARMPLL_BL0_CON1		0x21c
#define ARMPLL_BL1_CON1		0x22c
#define ARMPLL_BL2_CON1		0x23c
#define ARMPLL_BL3_CON1		0x24c

#define ARMPLL_CON0_EN		BIT(0)
#define ARMPLL_CON1_PCW_MASK	GENMASK(21, 0)
#define ARMPLL_CON1_POSDIV_MASK	GENMASK(26, 24)
#define ARMPLL_CON1_PCW_CHG	BIT(31)

#define MT6893_PLL_FMAX		(3800UL * HZ_PER_MHZ)
#define MT6893_PLL_FMIN		(1500UL * HZ_PER_MHZ)
#define MT6893_PLL_PCW_BITS	22
#define MT6893_PLL_PCW_IBITS	8
#define MT6893_PLL_PCW_FBITS	(MT6893_PLL_PCW_BITS - MT6893_PLL_PCW_IBITS)
#define MT6893_PLL_LOCK_US	20

static const u32 armpll_bl_con1_regs[] = {
	ARMPLL_BL0_CON1,
	ARMPLL_BL1_CON1,
	ARMPLL_BL2_CON1,
	ARMPLL_BL3_CON1,
};

static const u32 cpu_ll_mux_regs[] = {
	CPU_PLLDIV_CFG0,
};

static const u32 cpu_bl_mux_regs[] = {
	CPU_PLLDIV_CFG1,
	CPU_PLLDIV_CFG2,
	CPU_PLLDIV_CFG3,
	CPU_PLLDIV_CFG4,
};

static const u32 cpu_bus_mux_regs[] = {
	BUS_PLLDIV_CFG,
};

struct mt6893_bl_gang {
	struct clk_hw hw;
	struct regmap *apmixed;
};

struct mt6893_mcu_mux {
	struct clk_hw hw;
	struct regmap *regmap;
	const u32 *regs;
	unsigned int num_regs;
};

static inline struct mt6893_bl_gang *to_mt6893_bl_gang(struct clk_hw *hw)
{
	return container_of(hw, struct mt6893_bl_gang, hw);
}

static inline struct mt6893_mcu_mux *to_mt6893_mcu_mux(struct clk_hw *hw)
{
	return container_of(hw, struct mt6893_mcu_mux, hw);
}

static void mt6893_bl_pll_calc(unsigned long rate, unsigned long fin,
			       u32 *pcw, u32 *postdiv)
{
	u64 tmp;
	u32 val;

	if (rate > MT6893_PLL_FMAX)
		rate = MT6893_PLL_FMAX;

	for (val = 0; val < 5; val++) {
		*postdiv = 1U << val;
		if ((u64)rate * (*postdiv) >= MT6893_PLL_FMIN)
			break;
	}

	tmp = ((u64)rate << val) << MT6893_PLL_PCW_FBITS;
	do_div(tmp, fin);
	*pcw = (u32)tmp;
}

static unsigned long mt6893_bl_pll_recalc(unsigned long fin,
					  u32 pcw, u32 postdiv)
{
	u64 vco = (u64)fin * pcw;
	u8 carry = (vco & GENMASK_ULL(MT6893_PLL_PCW_FBITS - 1, 0)) ? 1 : 0;

	vco >>= MT6893_PLL_PCW_FBITS;
	if (carry)
		vco++;

	return div_u64(vco + postdiv - 1, postdiv);
}

static int mt6893_bl_gang_is_prepared(struct clk_hw *hw)
{
	struct mt6893_bl_gang *gang = to_mt6893_bl_gang(hw);
	u32 val = 0;

	regmap_read(gang->apmixed, ARMPLL_BL0_CON0, &val);
	return !!(val & ARMPLL_CON0_EN);
}

static unsigned long mt6893_bl_gang_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct mt6893_bl_gang *gang = to_mt6893_bl_gang(hw);
	u32 val = 0;
	u32 postdiv, pcw;

	regmap_read(gang->apmixed, ARMPLL_BL0_CON1, &val);
	postdiv = 1U << FIELD_GET(ARMPLL_CON1_POSDIV_MASK, val);
	pcw = FIELD_GET(ARMPLL_CON1_PCW_MASK, val);

	return mt6893_bl_pll_recalc(parent_rate, pcw, postdiv);
}

static int mt6893_bl_gang_determine_rate(struct clk_hw *hw,
					 struct clk_rate_request *req)
{
	u32 pcw, postdiv;

	if (!req->best_parent_rate)
		return -EINVAL;

	mt6893_bl_pll_calc(req->rate, req->best_parent_rate, &pcw, &postdiv);
	req->rate = mt6893_bl_pll_recalc(req->best_parent_rate, pcw, postdiv);

	return 0;
}

static int mt6893_bl_gang_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	struct mt6893_bl_gang *gang = to_mt6893_bl_gang(hw);
	u32 pcw, postdiv, val;
	int i, ret;

	if (!parent_rate)
		return -EINVAL;

	mt6893_bl_pll_calc(rate, parent_rate, &pcw, &postdiv);

	for (i = 0; i < ARRAY_SIZE(armpll_bl_con1_regs); i++) {
		ret = regmap_read(gang->apmixed, armpll_bl_con1_regs[i], &val);
		if (ret)
			return ret;

		val &= ~(ARMPLL_CON1_POSDIV_MASK | ARMPLL_CON1_PCW_MASK |
			 ARMPLL_CON1_PCW_CHG);
		val |= FIELD_PREP(ARMPLL_CON1_POSDIV_MASK, ffs(postdiv) - 1);
		val |= FIELD_PREP(ARMPLL_CON1_PCW_MASK, pcw);

		ret = regmap_write(gang->apmixed, armpll_bl_con1_regs[i], val);
		if (ret)
			return ret;

		ret = regmap_write(gang->apmixed, armpll_bl_con1_regs[i],
				   val | ARMPLL_CON1_PCW_CHG);
		if (ret)
			return ret;
	}

	/* Wait for all four ARMPLL_BL0..3 to lock in parallel */
	udelay(MT6893_PLL_LOCK_US);

	return 0;
}

static const struct clk_ops mt6893_bl_gang_ops = {
	.is_prepared = mt6893_bl_gang_is_prepared,
	.recalc_rate = mt6893_bl_gang_recalc_rate,
	.determine_rate = mt6893_bl_gang_determine_rate,
	.set_rate = mt6893_bl_gang_set_rate,
};

static u8 mt6893_mcu_mux_get_parent(struct clk_hw *hw)
{
	struct mt6893_mcu_mux *mux = to_mt6893_mcu_mux(hw);
	u32 val = 0;

	regmap_read(mux->regmap, mux->regs[0], &val);
	return FIELD_GET(CPU_PLLDIV_MUX_MASK, val);
}

static int mt6893_mcu_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct mt6893_mcu_mux *mux = to_mt6893_mcu_mux(hw);
	u32 val = FIELD_PREP(CPU_PLLDIV_MUX_MASK, index);
	u32 dummy;
	int i, ret;

	for (i = 0; i < mux->num_regs; i++) {
		ret = regmap_update_bits(mux->regmap, mux->regs[i],
					 CPU_PLLDIV_MUX_MASK, val);
		if (ret)
			return ret;
	}

	/* Ensure mux switch completes in MCUCFG before PLL retuning */
	regmap_read(mux->regmap, mux->regs[mux->num_regs - 1], &dummy);
	udelay(1);

	return 0;
}

static const struct clk_ops mt6893_mcu_mux_ops = {
	.determine_rate = clk_hw_determine_rate_no_reparent,
	.get_parent = mt6893_mcu_mux_get_parent,
	.set_parent = mt6893_mcu_mux_set_parent,
};

static struct clk_hw *
mt6893_register_mcu_mux(struct device *dev, struct regmap *regmap,
			const char *name, const struct clk_parent_data *parents,
			u8 num_parents, const u32 *regs, unsigned int num_regs)
{
	struct clk_init_data init = {};
	struct mt6893_mcu_mux *mux;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &mt6893_mcu_mux_ops;
	init.parent_data = parents;
	init.num_parents = num_parents;
	init.flags = CLK_SET_RATE_NO_REPARENT | CLK_IS_CRITICAL;

	mux->regmap = regmap;
	mux->regs = regs;
	mux->num_regs = num_regs;
	mux->hw.init = &init;

	ret = devm_clk_hw_register(dev, &mux->hw);
	if (ret)
		return ERR_PTR(ret);

	return &mux->hw;
}

static const struct clk_parent_data bl_gang_parents[] = {
	{ .fw_name = "clk26m", .name = "clk26m" },
};

static const struct clk_parent_data mcu_armpll_ll_parents[] = {
	{ .fw_name = "clk26m", .name = "clk26m" },
	{ .fw_name = "armpll_ll", .name = "armpll_ll" },
	{ .fw_name = "mainpll", .name = "mainpll" },
	{ .fw_name = "univpll", .name = "univpll" },
};

static const struct clk_parent_data mcu_armpll_bus_parents[] = {
	{ .fw_name = "clk26m", .name = "clk26m" },
	{ .fw_name = "ccipll", .name = "ccipll" },
	{ .fw_name = "mainpll", .name = "mainpll" },
	{ .fw_name = "univpll", .name = "univpll" },
};

static const char * const mt6893_mcucfg_req_clks[] = {
	"clk26m",
	"armpll_ll",
	"ccipll",
	"mainpll",
	"univpll",
};

static int clk_mt6893_mcucfg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct clk_parent_data bl_mux_parents[4] = {
		{ .fw_name = "clk26m", .name = "clk26m" },
		{ },
		{ .fw_name = "mainpll", .name = "mainpll" },
		{ .fw_name = "univpll", .name = "univpll" },
	};
	struct clk_hw_onecell_data *clk_data;
	struct clk_init_data gang_init = {};
	struct mt6893_bl_gang *bl_gang;
	struct regmap *mcusys, *apmixed;
	struct clk_hw *hw;
	int i, ret;

	/* Defer until clk26m and apmixedsys PLLs have registered */
	for (i = 0; i < ARRAY_SIZE(mt6893_mcucfg_req_clks); i++) {
		struct clk *clk = devm_clk_get(dev, mt6893_mcucfg_req_clks[i]);

		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk),
					     "failed to get %s clock\n",
					     mt6893_mcucfg_req_clks[i]);
	}

	mcusys = device_node_to_regmap(node);
	if (IS_ERR(mcusys))
		return dev_err_probe(dev, PTR_ERR(mcusys),
				     "failed to get mcusys regmap\n");

	apmixed = syscon_regmap_lookup_by_phandle(node, "mediatek,apmixedsys");
	if (IS_ERR(apmixed))
		return dev_err_probe(dev, PTR_ERR(apmixed),
				     "failed to get apmixedsys regmap\n");

	clk_data = mtk_devm_alloc_clk_data(dev, CLK_MCU_NR_CLK);
	if (!clk_data)
		return -ENOMEM;

	bl_gang = devm_kzalloc(dev, sizeof(*bl_gang), GFP_KERNEL);
	if (!bl_gang)
		return -ENOMEM;

	gang_init.name = "armpll_bl_gang";
	gang_init.ops = &mt6893_bl_gang_ops;
	gang_init.parent_data = bl_gang_parents;
	gang_init.num_parents = ARRAY_SIZE(bl_gang_parents);
	gang_init.flags = CLK_IS_CRITICAL;

	bl_gang->apmixed = apmixed;
	bl_gang->hw.init = &gang_init;

	ret = devm_clk_hw_register(dev, &bl_gang->hw);
	if (ret)
		return ret;

	clk_data->hws[CLK_MCU_ARMPLL_BL_GANG] = &bl_gang->hw;
	bl_mux_parents[1].hw = &bl_gang->hw;

	hw = mt6893_register_mcu_mux(dev, mcusys, "mcu_armpll_ll_sel",
				     mcu_armpll_ll_parents,
				     ARRAY_SIZE(mcu_armpll_ll_parents),
				     cpu_ll_mux_regs,
				     ARRAY_SIZE(cpu_ll_mux_regs));
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clk_data->hws[CLK_MCU_ARMPLL_LL_SEL] = hw;

	hw = mt6893_register_mcu_mux(dev, mcusys, "mcu_armpll_bl_sel",
				     bl_mux_parents,
				     ARRAY_SIZE(bl_mux_parents),
				     cpu_bl_mux_regs,
				     ARRAY_SIZE(cpu_bl_mux_regs));
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clk_data->hws[CLK_MCU_ARMPLL_BL_SEL] = hw;

	hw = mt6893_register_mcu_mux(dev, mcusys, "mcu_armpll_bus_sel",
				     mcu_armpll_bus_parents,
				     ARRAY_SIZE(mcu_armpll_bus_parents),
				     cpu_bus_mux_regs,
				     ARRAY_SIZE(cpu_bus_mux_regs));
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	clk_data->hws[CLK_MCU_ARMPLL_BUS_SEL] = hw;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
}

static const struct of_device_id of_match_clk_mt6893_mcucfg[] = {
	{ .compatible = "mediatek,mt6893-mcucfg" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_clk_mt6893_mcucfg);

static struct platform_driver clk_mt6893_mcucfg_drv = {
	.probe = clk_mt6893_mcucfg_probe,
	.driver = {
		.name = "clk-mt6893-mcucfg",
		.of_match_table = of_match_clk_mt6893_mcucfg,
	},
};
module_platform_driver(clk_mt6893_mcucfg_drv);

MODULE_DESCRIPTION("MediaTek MT6893 MCUCFG CPU/DSU clocks driver");
MODULE_LICENSE("GPL");
